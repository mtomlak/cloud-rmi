<?php
require_once __DIR__ . '/../../class/RemoteMethodInvocation/ConnectionInterface.php';
require_once __DIR__ . '/../../class/RemoteMethodInvocation/RemoteException.php';
require_once __DIR__ . '/../../class/RemoteMethodInvocation/RemoteErrorException.php';
require_once __DIR__ . '/../../class/RemoteMethodInvocation/RemoteMethodInvocationException.php';
require_once __DIR__ . '/../../class/RemoteMethodInvocation/SimpleRMIConnection.php';
require_once __DIR__ . '/../../class/RemoteMethodInvocation/SimpleRMIClient.php';
require_once __DIR__ . '/../../class/RemoteMethodInvocation/SimpleRMIServer.php';
require_once __DIR__ . '/../../class/RemoteMethodInvocation/SimplePreforkRMIServer.php';

use RemoteMethodInvocation\SimpleRMIClient;
use RemoteMethodInvocation\SimpleRMIConnection;
use RemoteMethodInvocation\RemoteErrorException;

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
    $cmd = [
        'php',
        '-d', "extension={$extPath}",
        __DIR__ . '/../../src/server.php',
        '--port=' . $port,
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
    fwrite(STDERR, "Extension not found at {$extPath}, skipping integration tests.\n");
    exit(0);
}

$port = 14000 + random_int(0, 1000);
[$proc, $pipes] = start_server($extPath, $port);
wait_for_server('127.0.0.1', $port);

try {
    $client = new SimpleRMIClient('127.0.0.1', $port);
    $response = $client->call('iface', 'method', 'hello');
    if ($response !== 'OK') {
        throw new RuntimeException('legacy call failed: ' . $response);
    }

    $payloadClient = new SimpleRMIClient('127.0.0.1', $port);
    $payloadResponse = $payloadClient->call('iface', 'upload', "bin\0bytes", ['payload' => true, 'encoding' => 'BLOB']);
    if ($payloadResponse !== 'OK') {
        throw new RuntimeException('payload call failed');
    }

    // mix modes on one connection
    $mixed = new SimpleRMIClient('127.0.0.1', $port);
    $r1 = $mixed->call('iface', 'method', 'first');
    $r2 = $mixed->call('iface', 'upload', 'second', ['payload' => true]);
    if ($r1 !== 'OK' || $r2 !== 'OK') {
        throw new RuntimeException('mixed call sequence failed');
    }

    // invalid PAYLOAD sequence
    $sock = new SimpleRMIConnection('127.0.0.1', $port);
    $sock->write("PAYLOAD RAW 2\nzz");
    $line = $sock->readLine();
    if ($line === false || strpos($line, '400') === false) {
        throw new RuntimeException('expected error for payload without call');
    }

    // legacy CALL followed by stray PAYLOAD
    $sock2 = new SimpleRMIConnection('127.0.0.1', $port);
    $sock2->write("CALL iface method AAAA\nPAYLOAD RAW 1\nx");
    $line2 = $sock2->readLine();
    if ($line2 === false || strpos($line2, '400') === false) {
        throw new RuntimeException('expected error for mixed legacy/payload');
    }

    fwrite(STDOUT, "SimpleRMIClient integration tests passed\n");
} finally {
    stop_server($proc, $pipes);
}
