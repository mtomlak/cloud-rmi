<?php
require_once __DIR__ . '/../support/test_harness.php';

use ProfiWeb\CloudRMI\CloudRMIClient;

cloudrmi_set_suite('simple_rmi_client');

[$proc, $pipes, $port] = cloudrmi_start_php_server();
$exitCode = 0;

try {
	cloudrmi_wait_for_server('127.0.0.1', $port);

	cloudrmi_run_test('legacy-call', function () use ($port) {
		$client = new CloudRMIClient('127.0.0.1', $port);
		$client->connect();
		$response = $client->call('TestRmiInterface', 'method', 'hello');
		$client->disconnect();
		if ($response !== 'OK') {
			throw new RuntimeException('legacy call failed: ' . $response);
		}
	});

	cloudrmi_run_test('payload-call', function () use ($port) {
		$payloadClient = new CloudRMIClient('127.0.0.1', $port);
		$payloadClient->connect();
		$payloadResponse = $payloadClient->call('TestRmiInterface', 'upload', "bin\0bytes", ['payload' => true, 'encoding' => 'RAW']);
		$payloadClient->disconnect();
		if ($payloadResponse !== 'OK') {
			throw new RuntimeException('payload call failed');
		}
	});

	cloudrmi_run_test('mixed-inline-and-payload', function () use ($port) {
		$mixed = new CloudRMIClient('127.0.0.1', $port);
		$mixed->connect();
		$r1 = $mixed->call('TestRmiInterface', 'method', 'first');
		$r2 = $mixed->call('TestRmiInterface', 'upload', 'second', ['payload' => true]);
		$mixed->disconnect();
		if ($r1 !== 'OK' || $r2 !== 'OK') {
			throw new RuntimeException('mixed call sequence failed');
		}
	});

	cloudrmi_run_test('payload-without-call-rejected', function () use ($port) {
		$sock = stream_socket_client("tcp://127.0.0.1:{$port}");
		fgets($sock, 2048);
		fwrite($sock, "PAYLOAD RAW 2\nzz");
		$line = fgets($sock, 2048);
		fclose($sock);
		if ($line === false || strpos($line, '400') === false) {
			throw new RuntimeException('expected error for payload without call');
		}
	});

	cloudrmi_run_test('mixed-legacy-and-payload-rejected', function () use ($port) {
		$sock2 = stream_socket_client("tcp://127.0.0.1:{$port}");
		fgets($sock2, 2048);
		fwrite($sock2, "CALL TestRmiInterface method AAAA\nPAYLOAD RAW 1\nx");
		$line2 = fgets($sock2, 2048);
		$line3 = fgets($sock2, 2048);
		fclose($sock2);
		$hasError = (strpos((string) $line2, '400') !== false) || (strpos((string) $line3, '400') !== false);
		if (!$hasError) {
			throw new RuntimeException('expected error for mixed legacy/payload');
		}
	});
} catch (Throwable $e) {
	$exitCode = 1;
} finally {
	cloudrmi_stop_process($proc, $pipes);
	cloudrmi_print_timing_summary();
}

exit($exitCode);
