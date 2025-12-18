<?php
function wait_for_server(string $host, int $port, int $retries = 20): void
{
    for ($i = 0; $i < $retries; $i++) {
        $sock = @stream_socket_client("tcp://{$host}:{$port}", $errno, $errstr, 0.2);
        if ($sock !== false) {
            fclose($sock);
            return;
        }
        usleep(100000);
    }
    throw new RuntimeException('server did not start');
}

function start_server(string $extPath, int $port): array
{
    $serverScript = realpath(__DIR__ . '/../support/test_server.php');
    if ($serverScript === false) {
        throw new RuntimeException('failed to locate test server script');
    }
    $extension = realpath($extPath) ?: $extPath;
    $cmd = [
        'php',
        '-d', "extension={$extension}",
        $serverScript,
        '--host=127.0.0.1',
        '--port=' . $port,
        '--proto-max=2',
        '--enable-fork=1',
    ];
    $descriptors = [
        0 => ['pipe', 'r'],
        1 => ['pipe', 'w'],
        2 => ['pipe', 'w'],
    ];
    $proc = proc_open($cmd, $descriptors, $pipes);
    if (!is_resource($proc)) {
        throw new RuntimeException('failed to start server');
    }
    stream_set_blocking($pipes[1], false);
    stream_set_blocking($pipes[2], false);
    return [$proc, $pipes];
}

function stop_server($proc, array $pipes): void
{
    foreach ($pipes as $pipe) {
        fclose($pipe);
    }
    proc_terminate($proc);
    proc_close($proc);
}

$extPath = __DIR__ . '/../../ext/profiweb_cloudrmi/modules/profiweb_cloudrmi.so';
if (!file_exists($extPath)) {
    fwrite(STDERR, "Extension not found at {$extPath}\n");
    exit(1);
}

$port = 15000 + random_int(0, 1000);
[$proc, $pipes] = start_server($extPath, $port);

try {
    wait_for_server('127.0.0.1', $port);

    $first = stream_socket_client("tcp://127.0.0.1:{$port}");
    if ($first === false) {
        throw new RuntimeException('failed to open first connection');
    }
    stream_set_timeout($first, 1);
    $banner1 = fgets($first, 2048);
    if ($banner1 === false || strpos($banner1, '220') !== 0) {
        throw new RuntimeException('did not receive banner on first connection');
    }

    $second = stream_socket_client("tcp://127.0.0.1:{$port}");
    if ($second === false) {
        throw new RuntimeException('failed to open second connection');
    }
    stream_set_timeout($second, 1);
    $banner2 = fgets($second, 2048);
    if ($banner2 === false || strpos($banner2, '220') !== 0) {
        throw new RuntimeException('second connection did not receive banner while first is open');
    }
    fwrite($second, "HELP\n");
    $helpLine = fgets($second, 2048);
    if ($helpLine === false || strpos($helpLine, '200') !== 0) {
        throw new RuntimeException('second connection did not respond to HELP');
    }

    fclose($first); // abrupt close without QUIT

    $third = stream_socket_client("tcp://127.0.0.1:{$port}");
    if ($third === false) {
        throw new RuntimeException('failed to open third connection');
    }
    stream_set_timeout($third, 1);
    $banner3 = fgets($third, 2048);
    if ($banner3 === false || strpos($banner3, '220') !== 0) {
        throw new RuntimeException('third connection did not receive banner after abrupt close');
    }
    fwrite($third, "NOOP\n");
    $noopLine = fgets($third, 2048);
    if ($noopLine === false || strpos($noopLine, '200') !== 0) {
        throw new RuntimeException('third connection did not respond to NOOP');
    }

    fclose($second);
    fclose($third);

    fwrite(STDOUT, "Concurrent connection tests passed\n");
} finally {
    stop_server($proc, $pipes);
}
