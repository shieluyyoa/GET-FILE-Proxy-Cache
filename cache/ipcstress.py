#!/usr/bin/python3

""" IPC Stress Test """

from typing import List, Tuple

import os
import shutil
import sys
import glob
import subprocess
import time
import re

# gfclient_download maximum request count
MAX_GFCLIENT_DOWNLOAD_REQUEST_COUNT = 1000

# Block size for the dd command.
DD_BLOCK_SIZE = 16

# Use some powers of two plus some multiple of the dd block size,
# to make generation reasonably fast.
#
# Bytes per second estimation is only available if request count is an even multiple of
# the workload sizes, thus the 10 sizes here.
# Note: A non-existent filename was added to the workload file, so all 
# request counts were changed to multiples of 11.
WORKLOAD_SIZES = [
    0,
    563,
    1024 + 1 * DD_BLOCK_SIZE,
    4096 + 7 * DD_BLOCK_SIZE,
    65536 + 13 * DD_BLOCK_SIZE,
    262144 + 19 * DD_BLOCK_SIZE,
    1048576 + 23 * DD_BLOCK_SIZE,
    4 * 1048576 + 29 * DD_BLOCK_SIZE,
    8 * 1048576 + 31 * DD_BLOCK_SIZE,
   16 * 1048576 + 33 * DD_BLOCK_SIZE,
]

# Alternative: random sizes

# For locals.txt, for simplecached:
WORKLOAD_LOCAL_PATH = 'ipcstress_files'

# For workload.txt, for gfclient_download to store:
WORKLOAD_URL_PATH = 'ipcstress'

LOCALS_FILENAME = 'locals-ipcstress.txt'
WORKLOAD_FILENAME = 'workload-ipcstress.txt'

# Minimum size of the shared memory to use in the tests
# This value has been know to change from semester to semester
MIN_SEG_SIZE = 824

def run_sha1sum(filenames: List[str], output_file: str) -> None:
    """ Run sha1sum on filenames and write to an output file. """
    # Get the hashes. SHA1 is good enough for this.
    result = subprocess.run(
        ['/usr/bin/sha1sum'] + filenames,
        capture_output=True,
        check=True)

    # Store the hashes.
    with open(output_file, 'wb') as file:
        file.write(result.stdout)


def create_workload(workdir: str):
    """ Create workload. """

    # Create path or ignore if already present.
    path = f'{workdir}/{WORKLOAD_LOCAL_PATH}'
    os.makedirs(path, exist_ok=True)

    # Create the files with random content.
    filenames = []
    print('Creating workload data files:')
    for i, size in enumerate(WORKLOAD_SIZES):
        filename = f'{path}/workload{i}.bin'
        filenames.append(filename)
        nblocks = size // DD_BLOCK_SIZE
        if (not (os.path.isfile(filename))):
            dd_result = subprocess.run([
                '/usr/bin/dd',
                'if=/dev/urandom',
                f'of={filename}',
                f'bs={DD_BLOCK_SIZE}',
                f'count={nblocks}'
            ], check=True, capture_output=True)
            print(dd_result.stderr.decode())

    full_sha1sum_filename = f'{path}/sha1sum.txt'
    print(f'Creating SHA1 hash file: {full_sha1sum_filename}')
    run_sha1sum(filenames, full_sha1sum_filename)

    # Create the locals file.
    full_locals_filename = f'{workdir}/{LOCALS_FILENAME}'
    print(f'Creating locals file: {full_locals_filename}')
    with open(full_locals_filename, 'w') as file:
        for i, filename in enumerate(filenames):
            file.write(f'/{WORKLOAD_URL_PATH}/workload{i}.bin {filename}\n')

    # Create the workload file.
    full_workload_filename = f'{workdir}/{WORKLOAD_FILENAME}'
    print(f'Creating workload file: {full_workload_filename}')
    with open(f'{workdir}/{WORKLOAD_FILENAME}', 'w') as file:
        for i, _ in enumerate(filenames):
            file.write(f'/{WORKLOAD_URL_PATH}/workload{i}.bin\n')
        file.write(f'/{WORKLOAD_URL_PATH}/workload_FNF.bin\n')

    # Delete the result directory if it exists, gfclient_download will recreate it.
    shutil.rmtree(f'{workdir}/{WORKLOAD_URL_PATH}', ignore_errors=True)

def read_cpu_times(pid: int) -> Tuple[int, int]:
    """ Read utime (user time) and stime (system/kernel time) for a PID, in ticks. """
    with open(f'/proc/{pid}/stat', 'r') as file:
        entries = file.readline().rstrip().split(' ')
        return int(entries[13]), int(entries[14])


def run_ipcstress(
    workdir: str,
    cache_thread_count: int,
    proxy_thread_count: int,
    proxy_segment_count: int,
    proxy_segment_size: int,
    download_thread_count: int,
    request_count: int,
    port: int
) -> int:
    """ Run IPC Stress. Return 0 for normal exit. """

    # Compute the ticks per second
    result = subprocess.run(['/usr/bin/getconf', 'CLK_TCK'], capture_output=True, check=True)
    ticks_per_second = int(result.stdout)

    remaining_request_count = request_count

    popen_cache = subprocess.Popen([
        f'{os.getcwd()}/simplecached',
        '-c',
        f'./{LOCALS_FILENAME}',
        '-t',
        str(cache_thread_count)
    ], cwd=workdir
    )
    # print(f'cache pid: {popen_cache.pid}')

    popen_proxy = subprocess.Popen([
        f'{os.getcwd()}/webproxy',
        '-n',
        str(proxy_segment_count),
        '-p',
        str(port),
        '-t',
        str(proxy_thread_count),
        '-z',
        str(proxy_segment_size)
    ], cwd=workdir
    )
    # print(f'proxy pid: {popen_proxy.pid}')

    # Give the proxy a quarter second to start, to eliminate the client message:
    # Failed to connect.  Trying again....
    time.sleep(0.250)

    actual_request_done = 0
    popen_download = None
    download_poll = None

    # Benchmarking:
    start_time = None
    start_cache_utime, start_cache_stime = read_cpu_times(popen_cache.pid)
    start_proxy_utime, start_proxy_stime = read_cpu_times(popen_proxy.pid)

    # Summary for the end.
    total_elapsed_time = 0
    total_elapsed_cache_utime = 0
    total_elapsed_cache_stime = 0
    total_elapsed_proxy_utime = 0
    total_elapsed_proxy_stime = 0
    
    # print(f'download pid: {popen_download.pid}')
    while True:
        if popen_download:
            download_poll = popen_download.poll()

        # Download if first time or previous request complete.
        # explicit "is not None" is needed because the return code may be 0
        if (download_poll is not None) or not popen_download:
            if start_time:
                elapsed_time = time.time() - start_time

                # Requests per second
                rps = actual_request_count / elapsed_time
                actual_request_done += actual_request_count

                # CPU time (user and system)
                cache_utime, cache_stime = read_cpu_times(popen_cache.pid)
                proxy_utime, proxy_stime = read_cpu_times(popen_proxy.pid)
                elapsed_cache_utime = (cache_utime - start_cache_utime) / ticks_per_second
                elapsed_cache_stime = (cache_stime - start_cache_stime) / ticks_per_second
                elapsed_proxy_utime = (proxy_utime - start_proxy_utime) / ticks_per_second
                elapsed_proxy_stime = (proxy_stime - start_proxy_stime) / ticks_per_second
                (start_cache_utime, start_cache_stime) = (cache_utime, cache_stime)
                (start_proxy_utime, start_proxy_stime) = (proxy_utime, proxy_stime)

                elapsed_cache_ttime = elapsed_cache_utime + elapsed_cache_stime
                elapsed_proxy_ttime = elapsed_proxy_utime + elapsed_proxy_stime

                # For the summary:
                total_elapsed_cache_utime += elapsed_cache_utime
                total_elapsed_cache_stime += elapsed_cache_stime
                total_elapsed_proxy_utime += elapsed_proxy_utime
                total_elapsed_proxy_stime += elapsed_proxy_stime

                # bps is only possible if the requests are a multiple of the workload.
                # Otherwise, gfclient_download does not evenly distribute the requests
                # across the workload files.
                request_count_chunk, request_count_extra = divmod(actual_request_count, len(WORKLOAD_SIZES))
                if not request_count_extra:
                    nbytes = request_count_chunk * sum(WORKLOAD_SIZES)
                    bps = nbytes / elapsed_time
                    print(
                        f'{actual_request_done}/{request_count} in {elapsed_time:0.2f}s, {rps:0.2f} rps, {bps:0.0f} bps, ',
                        end=''
                    )
                else:
                    print(
                        f'{actual_request_done}/{request_count} in {elapsed_time:0.2f}s, {rps:0.2f} rps, ',
                        end=''
                    )

                print(
                    'cache: '
                    f'{elapsed_cache_utime}s {100 * elapsed_cache_utime / elapsed_time:0.2f}% user, '
                    f'{elapsed_cache_stime}s {100 * elapsed_cache_stime / elapsed_time:0.2f}% kernel, '
                    f'{elapsed_cache_ttime}s {100 * elapsed_cache_ttime / elapsed_time:0.2f}% total, '
                    'proxy: '
                    f'{elapsed_proxy_utime}s {100 * elapsed_proxy_utime / elapsed_time:0.2f}% user, '
                    f'{elapsed_proxy_stime}s {100 * elapsed_proxy_stime / elapsed_time:0.2f}% kernel, '
                    f'{elapsed_proxy_ttime}s {100 * elapsed_proxy_ttime / elapsed_time:0.2f}% total'
                )

            if remaining_request_count == 0:
                break

            actual_request_count = min(MAX_GFCLIENT_DOWNLOAD_REQUEST_COUNT, remaining_request_count)

            popen_download = subprocess.Popen([
                f'{os.getcwd()}/gfclient_download',
                '-p',
                str(port),
                '-t',
                str(download_thread_count),
                '-w',
                f'./{WORKLOAD_FILENAME}',
                '-r',
                str(actual_request_count)
            ], cwd=workdir
            )
            remaining_request_count -= actual_request_count
            start_time = time.time()

        cache_poll = popen_cache.poll() 
        proxy_poll = popen_proxy.poll()

        print(f'cache poll : {cache_poll}')
        print(f'proxy poll : {proxy_poll}')

        if (cache_poll is not None) and (proxy_poll is not None):
            print(f'Both cache exited ({cache_poll}) and proxy ({proxy_poll}) exited')
            popen_download.terminate()
            return 3
        if cache_poll is not None:
            print(f'Cache exited ({cache_poll})')
            popen_download.terminate()
            popen_proxy.terminate()
            return 1
        if proxy_poll is not None:
            print(f'Proxy exited ({proxy_poll})')
            popen_download.terminate()
            popen_cache.terminate()
            return 2

        time.sleep(1)

    popen_cache.terminate()
    popen_proxy.terminate()

    # Benchmark for this run, if it ran more than once
    if total_elapsed_time > elapsed_time:
        rps = actual_request_done / total_elapsed_time
        total_elapsed_cache_ttime = total_elapsed_cache_utime + total_elapsed_cache_stime
        total_elapsed_proxy_ttime = total_elapsed_proxy_utime + total_elapsed_proxy_stime

        request_count_chunk, request_count_extra = divmod(actual_request_done, len(WORKLOAD_SIZES))
        if not request_count_extra:
            nbytes = request_count_chunk * sum(WORKLOAD_SIZES)
            bps = nbytes / total_elapsed_time
            print(
                f'Summary: {total_elapsed_time:0.2f}s, {rps:0.2f} rps, {bps:0.0f} bps, ',
                end=''
            )
        else:
            print(
                f'Summary: {total_elapsed_time:0.2f}s, {rps:0.2f} rps, ',
                end=''
            )

        print(
            'cache: '
            f'{total_elapsed_cache_utime}s {100 * total_elapsed_cache_utime / total_elapsed_time:0.2f}% user, '
            f'{total_elapsed_cache_stime}s {100 * total_elapsed_cache_stime / total_elapsed_time:0.2f}% kernel, '
            f'{total_elapsed_cache_ttime}s {100 * total_elapsed_cache_ttime / total_elapsed_time:0.2f}% total, '
            'proxy: '
            f'{total_elapsed_proxy_utime}s {100 * total_elapsed_proxy_utime / total_elapsed_time:0.2f}% user, '
            f'{total_elapsed_proxy_stime}s {100 * total_elapsed_proxy_stime / total_elapsed_time:0.2f}% kernel, '
            f'{total_elapsed_proxy_ttime}s {100 * total_elapsed_proxy_ttime / total_elapsed_time:0.2f}% total'
        )


    return 0


def verify_results(workdir: str) -> str:
    """ Verify results, return True on success. """
    filenames = glob.glob(f'{workdir}/{WORKLOAD_URL_PATH}/*')
    result_filename = f'{workdir}/{WORKLOAD_URL_PATH}/sha1sum-result.txt'
    run_sha1sum(filenames, result_filename)

    # Entries in the sha1sum files are full paths.
    re_sha1sum = re.compile(r'(\w+)\s+(.*)')

    # Load the original hashes for comparison.
    workload_sha1 = {}
    with open(f'{workdir}/{WORKLOAD_LOCAL_PATH}/sha1sum.txt', 'r') as file:
        for line in file:
            match = re_sha1sum.match(line.rstrip())
            if match:
                workload_sha1[os.path.basename(match.group(2))] = match.group(1)

    # Find all the mismatching hashes.
    success = True
    with open(result_filename, 'r') as file:
        for line in file:
            match = re_sha1sum.match(line.rstrip())
            if match:
                filename = os.path.basename(match.group(2))
                workload_hash = workload_sha1.get(filename)
                if workload_hash and (workload_hash != match.group(1)):
                    print(f'Hash mismatch: {filename}')
                    success = False

    return success


def run_base_test(workdir: str):
    """ Base level of testing. """

    port = 10826
    create_workload(workdir)

    request_count = 110
    cache_thread_count = 1
    proxy_thread_count = 1
    proxy_segment_count = 1
    proxy_segment_size = 1024
    download_thread_count = 1

    print(
        f'cache_thread_count={cache_thread_count}, proxy_thread_count={proxy_thread_count}, '
        f'proxy_segment_count={proxy_segment_count}, proxy_segment_size={proxy_segment_size}, '
        f'download_thread_count={download_thread_count}, request_count={request_count}'
    )
    if run_ipcstress(
        workdir,
        cache_thread_count,
        proxy_thread_count,
        proxy_segment_count,
        proxy_segment_size,
        download_thread_count,
        request_count,
        port
    ) != 0:
        return

    if not verify_results(workdir):
        return


def run_parameter_test(workdir: str):
    """ Test through a wide range of parameters. """

    port = 10825
    create_workload(workdir)

    request_count = 11
    for cache_thread_count in range(1, 101, 10):
        for proxy_thread_count in range(cache_thread_count, 101, 10):
            for proxy_segment_count in range(1, 101, 10):
                download_thread_count = proxy_thread_count

                proxy_segment_size = MIN_SEG_SIZE
                while proxy_segment_size <= 1048576:
                    print(
                        f'cache_thread_count={cache_thread_count}, proxy_thread_count={proxy_thread_count}, '
                        f'proxy_segment_count={proxy_segment_count}, proxy_segment_size={proxy_segment_size}, '
                        f'download_thread_count={download_thread_count}, request_count={request_count}'
                    )

                    if run_ipcstress(
                        workdir,
                        cache_thread_count,
                        proxy_thread_count,
                        proxy_segment_count,
                        proxy_segment_size,
                        download_thread_count,
                        request_count,
                        port
                    ) != 0:
                        return

                    if not verify_results(workdir):
                        return

                    proxy_segment_size *= 4


def run_stress_test(workdir: str):
    """ Stress test with fixed proxy segment size and number of segments. """

    port = 10824
    create_workload(workdir)

    request_count = 110
    proxy_segment_count = 50
    proxy_segment_size = 1048576
    # proxy_segment_size = 1024

    for cache_thread_count in range(20, 101, 10):
        for proxy_thread_count in range(cache_thread_count, 101, 10):
            download_thread_count = proxy_thread_count

            print(
                f'cache_thread_count={cache_thread_count}, proxy_thread_count={proxy_thread_count}, '
                f'proxy_segment_count={proxy_segment_count}, proxy_segment_size={proxy_segment_size}, '
                f'download_thread_count={download_thread_count}, request_count={request_count}'
            )

            if run_ipcstress(
                workdir,
                cache_thread_count,
                proxy_thread_count,
                proxy_segment_count,
                proxy_segment_size,
                download_thread_count,
                request_count,
                port
            ) != 0:
                return

            if not verify_results(workdir):
                return

def run_soak_test(workdir: str):
    """ Soak test with fixed parameters. """

    port = 10825
    create_workload(workdir)

    request_count = 1100000
    proxy_segment_count = 50
    proxy_segment_size = 1048576

    cache_thread_count = 100
    proxy_thread_count = 100
    download_thread_count = proxy_thread_count

    print(
        f'cache_thread_count={cache_thread_count}, proxy_thread_count={proxy_thread_count}, '
        f'proxy_segment_count={proxy_segment_count}, proxy_segment_size={proxy_segment_size}, '
        f'download_thread_count={download_thread_count}, request_count={request_count}'
    )

    if run_ipcstress(
        workdir,
        cache_thread_count,
        proxy_thread_count,
        proxy_segment_count,
        proxy_segment_size,
        download_thread_count,
        request_count,
        port
    ) != 0:
        return

    if not verify_results(workdir):
        return


if __name__ == '__main__':
    test_names = [
        name.split('_')[1] for name in globals().keys()
        if name.startswith('run_') and name.endswith("_test")
    ]

    print(f'python3 {sys.argv[0]} workdir {test_names}')
    workdir = sys.argv[1] if len(sys.argv) >= 2 else '.'
    test_name = sys.argv[2] if len(sys.argv) >= 3 else 'base'

    (globals()[f'run_{test_name}_test'])(workdir)
