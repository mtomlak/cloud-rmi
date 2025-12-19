<?php
require_once __DIR__ . '/../bootstrap.php';

use ProfiWeb\CloudRMI\CloudRMIClient;

if (!function_exists('pcntl_fork')) {
    fwrite(STDOUT, "pcntl extension missing, skipping ACALL capability tests\n");
    exit(0);
}

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

function start_server(int $port, bool $enableFork): array
{
    $cmd = cloudrmi_test_command(
        __DIR__ . '/../support/test_server.php',
        [
            '--host=127.0.0.1',
            '--port=' . $port,
            '--proto-max=2',
        ]
    );

    if ($enableFork) {
        $cmd[] = '--enable-fork=1';
    }

    $descriptors = [
        0 => ['pipe', 'r'],
        1 => ['pipe', 'w'],
        2 => ['pipe', 'w'],
    ];

    $proc = proc_open($cmd, $descriptors, $pipes);

    if (!is_resource($proc))
        throw new RuntimeException('failed to start server');

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

function read_banner(int $port): string
{
    $sock = @stream_socket_client("tcp://127.0.0.1:{$port}");

    if ($sock === false)
        throw new RuntimeException('unable to read banner');

    $line = fgets($sock, 2048);
    fclose($sock);

    if ($line === false)
        throw new RuntimeException('missing banner');

    return trim($line);
}

function read_caps(int $port): string
{
    $sock = @stream_socket_client("tcp://127.0.0.1:{$port}");

    if ($sock === false)
        throw new RuntimeException('unable to request CAPS');

    fgets($sock, 2048); // banner
    fwrite($sock, "CAPS\r\n");
    $line = fgets($sock, 2048);
    fclose($sock);

    if ($line === false)
        throw new RuntimeException('caps missing');

    return trim($line);
}

$asyncPort = 17000 + random_int(0, 200);
[$asyncProc, $asyncPipes] = start_server($asyncPort, true);
wait_for_server('127.0.0.1', $asyncPort);

try {
    $banner = read_banner($asyncPort);
    if (strpos($banner, 'ACALL') === false) {
        throw new RuntimeException('async capability not advertised in banner');
    }

    $capsLine = read_caps($asyncPort);
    if (strpos($capsLine, 'ACALL') === false) {
        throw new RuntimeException('CAPS did not include ACALL');
    }

    $async = new CloudRMIClient('127.0.0.1', $asyncPort);
    $async->connect();
    $response = $async->call('TestRmiInterface', 'method', 'async', ['async' => true]);
    $async->disconnect();

    if ($response !== 'Call Executed') {
        throw new RuntimeException('async ACALL response mismatch: ' . var_export($response, true));
    }
} finally {
    stop_server($asyncProc, $asyncPipes);
}

$fallbackPort = 18000 + random_int(0, 200);
[$fallbackProc, $fallbackPipes] = start_server($fallbackPort, false);
wait_for_server('127.0.0.1', $fallbackPort);

try {
    $bannerNoAsync = read_banner($fallbackPort);
    if (strpos($bannerNoAsync, 'ACALL') !== false) {
        throw new RuntimeException('async capability advertised without fork support');
    }

    $capsLineNoAsync = read_caps($fallbackPort);
    if (strpos($capsLineNoAsync, 'ACALL') !== false) {
        throw new RuntimeException('CAPS listed ACALL without fork support');
    }

    $client = new CloudRMIClient('127.0.0.1', $fallbackPort);
    $client->connect();
    $syncResponse = $client->call('TestRmiInterface', 'method', 'sync', ['async' => true]);
    $client->disconnect();

    if ($syncResponse !== 'OK') {
        throw new RuntimeException('async fallback did not return sync result');
    }
} finally {
    stop_server($fallbackProc, $fallbackPipes);
}

fwrite(STDOUT, "ACALL capability tests passed\n");
