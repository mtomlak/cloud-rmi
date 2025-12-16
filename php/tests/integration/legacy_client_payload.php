<?php
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/ConnectionInterface.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteException.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteErrorException.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteMethodInvocationException.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/SimpleRMIConnection.php';
require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/SimpleRMIClient.php';

use ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation\SimpleRMIClient;

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

function start_server(int $port)
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
    $legacy = new SimpleRMIClient('127.0.0.1', $port);
    $legacy->connect();
    $legacyResponse = $legacy->callRemoteMethod('TestRmiInterface', 'method', 'hello');
    $legacy->disconnect();
    if ($legacyResponse !== 'OK') {
        throw new RuntimeException('legacy call failed: ' . var_export($legacyResponse, true));
    }

    $payloadClient = new SimpleRMIClient('127.0.0.1', $port);
    $payloadClient->connect();
    $payloadResponse = $payloadClient->callRemoteMethod('TestRmiInterface', 'upload', "bin\0bytes", ['payload' => true, 'encoding' => 'RAW']);
    $payloadClient->disconnect();
    if ($payloadResponse !== 'OK') {
        throw new RuntimeException('payload call failed');
    }

    $binaryBody = random_bytes(8) . "\0" . random_bytes(4);
    $binary = new SimpleRMIClient('127.0.0.1', $port);
    $binary->connect();
    $binaryResponse = $binary->callRemoteMethod('TestRmiInterface', 'upload', $binaryBody, ['payload' => true]);
    $binary->disconnect();
    if ($binaryResponse !== 'OK') {
        throw new RuntimeException('binary payload call failed');
    }

    $mixed = new SimpleRMIClient('127.0.0.1', $port);
    $mixed->connect();
    $mixedLegacy = $mixed->callRemoteMethod('TestRmiInterface', 'method', 'first');
    $mixedPayload = $mixed->callRemoteMethod('TestRmiInterface', 'upload', 'second', ['payload' => true]);
    $mixed->disconnect();
    if ($mixedLegacy !== 'OK' || $mixedPayload !== 'OK') {
        throw new RuntimeException('mixed call sequence failed');
    }

    $sock = stream_socket_client("tcp://127.0.0.1:{$port}");
    fgets($sock, 2048); // banner
    fwrite($sock, "PAYLOAD RAW 2\nzz");
    $line = fgets($sock, 2048);
    fclose($sock);
    if ($line === false || strpos($line, '400') === false) {
        throw new RuntimeException('expected error for payload without call');
    }

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

    fwrite(STDOUT, "SimpleRMIClient payload integration tests passed\n");
} finally {
    stop_server($proc, $pipes);
}
