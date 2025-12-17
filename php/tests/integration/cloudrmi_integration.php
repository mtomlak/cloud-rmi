<?php
require_once __DIR__ . '/../../src/client.php';

function wait_for_server(string $host, int $port, int $retries = 10): void
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
    fwrite(STDERR, "Extension not found at {$extPath}\n");
    exit(1);
}

$port = 12000 + random_int(0, 1000);
[$proc, $pipes] = start_server($extPath, $port);
wait_for_server('127.0.0.1', $port);

try {
    // legacy call ok
    $sock = cloudrmi_connect('127.0.0.1', $port);
    cloudrmi_send_legacy_call($sock, 'iface', 'method', 'inline');
    $line = fgets($sock, 2048);
    if ($line === false || strpos($line, '200') !== 0) {
        throw new RuntimeException('legacy call failed');
    }
    fclose($sock);

	// payload call ok with binary data
	$sock = cloudrmi_connect('127.0.0.1', $port);
	fwrite($sock, "PROTO 2\n");
	$protoLine = fgets($sock, 2048);
	if ($protoLine === false || strpos($protoLine, '200') !== 0) {
		throw new RuntimeException('proto negotiation failed');
	}
	$payload = "bin\0bytes";
	cloudrmi_send_payload_call($sock, 'iface', 'upload', 'RAW', $payload);
	$payloadLine = fgets($sock, 2048);
    if ($payloadLine === false || strpos($payloadLine, '200') !== 0) {
        throw new RuntimeException('payload call failed');
    }
    fclose($sock);

    // invalid sequence triggers error
    $sock = cloudrmi_connect('127.0.0.1', $port);
    fwrite($sock, "PAYLOAD RAW 2\nab");
    $response = fgets($sock);
    if ($response === false || strpos($response, '400') === false) {
        throw new RuntimeException('expected error response');
    }
    fclose($sock);

    fwrite(STDOUT, "Integration tests passed\n");
} finally {
    stop_server($proc, $pipes);
}
