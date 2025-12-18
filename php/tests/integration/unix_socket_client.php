<?php
require_once __DIR__ . '/../support/extension_helpers.php';
ensure_cloudrmi_extension_loaded();

use ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation\CloudRMIClient;

function wait_for_unix_server(string $path, int $retries = 40): void
{
    for ($i = 0; $i < $retries; $i++) {
        if (file_exists($path)) {
            $sock = @stream_socket_client('unix://' . $path, $errno, $errstr, 0.2);
            if ($sock !== false) {
                fclose($sock);
                return;
            }
        }
        usleep(100000);
    }

    throw new RuntimeException('unix server did not start at ' . $path);
}

function start_unix_server(string $extPath, string $socketPath): array
{
    $cmd = array_merge(
        ['php'],
        cloudrmi_extension_cli($extPath),
        [
            __DIR__ . '/../support/test_server.php',
            '--unix-socket=' . $socketPath,
            '--proto-max=2',
        ]
    );
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

$extPath = cloudrmi_extension_path();
$socketPath = sys_get_temp_dir() . '/cloudrmi-' . uniqid('', true) . '.sock';
[$proc, $pipes] = start_unix_server($extPath, $socketPath);
wait_for_unix_server($socketPath);

try {
    $bannerSock = stream_socket_client('unix://' . $socketPath);
    $banner = fgets($bannerSock, 2048);
    fwrite($bannerSock, "PROTO 1\n");
    $downgrade = fgets($bannerSock, 2048);
    fclose($bannerSock);

    if (!is_string($banner) || strpos($banner, 'PROTO=2') === false) {
        throw new RuntimeException('unix socket banner did not advertise PROTO=2');
    }

    if (!is_string($downgrade) || strpos($downgrade, '400') === false) {
        throw new RuntimeException('expected downgrade attempt to be rejected');
    }

    $client = new CloudRMIClient('localhost', 0, false, 5, false, $socketPath);
    $client->connect();
    $response = $client->call('TestRmiInterface', 'method', 'hello');
    if ($response !== 'OK') {
        throw new RuntimeException('unix socket legacy call failed: ' . var_export($response, true));
    }

    $payloadResponse = $client->call('TestRmiInterface', 'upload', "bin\0bytes", ['payload' => true, 'encoding' => 'RAW']);
    $client->disconnect();

    if ($payloadResponse !== 'OK') {
        throw new RuntimeException('unix socket payload call failed');
    }

    fwrite(STDOUT, "CloudRMIClient unix socket integration tests passed\n");
} finally {
    stop_server($proc, $pipes);
    @unlink($socketPath);
}
