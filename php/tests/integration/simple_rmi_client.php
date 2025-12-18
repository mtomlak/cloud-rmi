<?php
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/ConnectionInterface.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteException.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteErrorException.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteMethodInvocationException.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/SimpleRMIConnection.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/CloudRMIClient.php';

use ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation\CloudRMIClient;
use ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation\RemoteErrorException;

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

function start_server(int $port): array
{
    $cmd = [
        'php',
        __DIR__ . '/../support/test_server.php',
        '--host=127.0.0.1',
        '--port=' . $port,
        '--proto-max=2',
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

$port = 14000 + random_int(0, 1000);
[$proc, $pipes] = start_server($port);
wait_for_server('127.0.0.1', $port);

try {
    $client = new CloudRMIClient('127.0.0.1', $port);
    $client->connect();
    $response = $client->call('TestRmiInterface', 'method', 'hello');
    $client->disconnect();
    if ($response !== 'OK') {
        throw new RuntimeException('legacy call failed: ' . $response);
    }

    $payloadClient = new CloudRMIClient('127.0.0.1', $port);
    $payloadClient->connect();
    $payloadResponse = $payloadClient->call('TestRmiInterface', 'upload', "bin\0bytes", ['payload' => true, 'encoding' => 'RAW']);
    $payloadClient->disconnect();
    if ($payloadResponse !== 'OK') {
        throw new RuntimeException('payload call failed');
    }

    // mix modes on one connection
    $mixed = new CloudRMIClient('127.0.0.1', $port);
    $mixed->connect();
    $r1 = $mixed->call('TestRmiInterface', 'method', 'first');
    $r2 = $mixed->call('TestRmiInterface', 'upload', 'second', ['payload' => true]);
    $mixed->disconnect();
    if ($r1 !== 'OK' || $r2 !== 'OK') {
        throw new RuntimeException('mixed call sequence failed');
    }

    // invalid PAYLOAD sequence
    $sock = stream_socket_client("tcp://127.0.0.1:{$port}");
    fgets($sock, 2048); // banner
    fwrite($sock, "PAYLOAD RAW 2\nzz");
    $line = fgets($sock, 2048);
    fclose($sock);
    if ($line === false || strpos($line, '400') === false) {
        throw new RuntimeException('expected error for payload without call');
    }

    // legacy CALL followed by stray PAYLOAD
    $sock2 = stream_socket_client("tcp://127.0.0.1:{$port}");
    fgets($sock2, 2048); // banner
    fwrite($sock2, "CALL TestRmiInterface method AAAA\nPAYLOAD RAW 1\nx");
    $line2 = fgets($sock2, 2048);
    $line3 = fgets($sock2, 2048);
    fclose($sock2);
    $hasError = (strpos((string)$line2, '400') !== false) || (strpos((string)$line3, '400') !== false);
    if (!$hasError) {
        throw new RuntimeException('expected error for mixed legacy/payload');
    }

    fwrite(STDOUT, "CloudRMIClient integration tests passed\n");
} finally {
    stop_server($proc, $pipes);
}
