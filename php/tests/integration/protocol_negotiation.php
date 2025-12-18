<?php
if (!extension_loaded('cloudrmi')) {
    require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/ConnectionInterface.php';
    require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteException.php';
    require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteErrorException.php';
    require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/RemoteMethodInvocationException.php';
    require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/SimpleRMIConnection.php';
    require_once __DIR__ . '/../../class/Messaging/RemoteMethodInvocation/CloudRMIClient.php';
}

use ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation\CloudRMIClient;

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

function start_test_server(int $port, int $maxProto): array
{
    $cmd = [
        'php',
        __DIR__ . '/../support/test_server.php',
        '--host=127.0.0.1',
        '--port=' . $port,
        '--proto-max=' . $maxProto,
    ];
    $descriptors = [
        0 => ['pipe', 'r'],
        1 => ['pipe', 'w'],
        2 => ['pipe', 'w'],
    ];
    $proc = proc_open($cmd, $descriptors, $pipes);
    if (!is_resource($proc)) {
        throw new RuntimeException('failed to start test server');
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

function read_banner(int $port): string
{
    $sock = @stream_socket_client("tcp://127.0.0.1:{$port}");
    if ($sock === false) {
        throw new RuntimeException('unable to connect to server for banner');
    }
    $line = fgets($sock, 2048);
    fclose($sock);
    if ($line === false) {
        throw new RuntimeException('server banner missing');
    }
    return trim($line);
}

$portV2 = 15000 + random_int(0, 500);
[$procV2, $pipesV2] = start_test_server($portV2, 2);
wait_for_server('127.0.0.1', $portV2);

try {
    $banner = read_banner($portV2);
    if (strpos($banner, 'PROTO=2') === false) {
        throw new RuntimeException('v2 server did not advertise proto 2');
    }

    $raw = stream_socket_client("tcp://127.0.0.1:{$portV2}");
    fgets($raw, 2048); // banner
    fwrite($raw, "PROTO 2\r\n");
    $protoLine = fgets($raw, 2048);
    if ($protoLine === false || strpos($protoLine, '200 PROTO 2') === false) {
        throw new RuntimeException('v2 proto negotiation failed');
    }

    fwrite($raw, "CALL TestRmiInterface upload RAW 3\r\nabc");
    $responseLine = fgets($raw, 2048);
    if ($responseLine === false || strpos($responseLine, '200 PAYLOAD RAW 2') === false) {
        throw new RuntimeException('payload response did not use PAYLOAD framing');
    }
    $payload = stream_get_contents($raw, 2);
    if ($payload !== 'OK') {
        throw new RuntimeException('payload response body mismatch: ' . var_export($payload, true));
    }
    fclose($raw);

    $clientV2 = new CloudRMIClient('127.0.0.1', $portV2);
    $clientV2->connect();
    $clientResult = $clientV2->call('TestRmiInterface', 'method', 'hello');
    if ($clientResult !== 'OK') {
        throw new RuntimeException('client inline call failed for v2');
    }

    $binary = "bin\0data";
    $payloadResult = $clientV2->call('TestRmiInterface', 'upload', $binary, ['payload' => true, 'encoding' => 'RAW']);
    if ($payloadResult !== 'OK') {
        throw new RuntimeException('client payload call failed for v2');
    }
    $clientV2->disconnect();
} finally {
    stop_server($procV2, $pipesV2);
}

$portV1 = 16000 + random_int(0, 500);
[$procV1, $pipesV1] = start_test_server($portV1, 1);
wait_for_server('127.0.0.1', $portV1);

try {
    $bannerV1 = read_banner($portV1);
    if (strpos($bannerV1, 'PROTO=1') === false) {
        throw new RuntimeException('v1-only server did not advertise proto 1');
    }

    $rawV1 = stream_socket_client("tcp://127.0.0.1:{$portV1}");
    fgets($rawV1, 2048); // banner
    fwrite($rawV1, "PROTO 2\r\n");
    $protoLineV1 = fgets($rawV1, 2048);
    if ($protoLineV1 === false || strpos($protoLineV1, '200 PROTO 1') === false) {
        throw new RuntimeException('v1 server did not clamp proto negotiation');
    }
    fclose($rawV1);

    $clientV1 = new CloudRMIClient('127.0.0.1', $portV1);
    $clientV1->connect();
    $legacy = $clientV1->call('TestRmiInterface', 'method', 'legacy');
    if ($legacy !== 'OK') {
        throw new RuntimeException('legacy client call failed on v1 server');
    }
    $clientV1->disconnect();
} finally {
    stop_server($procV1, $pipesV1);
}

fwrite(STDOUT, "Protocol negotiation tests passed\n");
