<?php
require_once __DIR__ . '/../support/test_harness.php';

use ProfiWeb\CloudRMI\CloudRMIClient;

if (!function_exists('pcntl_fork')) {
	echo "pcntl extension missing, skipping ACALL capability tests\n";
	exit(0);
}

cloudrmi_set_suite('acall_capability');

$exitCode = 0;

try {
	cloudrmi_run_test('acall-advertised-with-fork', function () {
		[$proc, $pipes, $port] = cloudrmi_start_php_server(null, ['worker_model' => 'fork']);
		try {
			cloudrmi_wait_for_server('127.0.0.1', $port);
			$bannerSock = stream_socket_client("tcp://127.0.0.1:{$port}");
			$banner = fgets($bannerSock, 2048);
			fclose($bannerSock);
			if ($banner === false || strpos($banner, 'ACALL') === false) {
				throw new RuntimeException('async capability not advertised in banner');
			}

			$sock = stream_socket_client("tcp://127.0.0.1:{$port}");
			fgets($sock, 2048);
			fwrite($sock, "CAPS\r\n");
			$capsLine = fgets($sock, 2048);
			fclose($sock);
			if ($capsLine === false || strpos($capsLine, 'ACALL') === false) {
				throw new RuntimeException('CAPS did not include ACALL');
			}

			$async = new CloudRMIClient('127.0.0.1', $port);
			$async->connect();
			$response = $async->call('TestRmiInterface', 'method', 'async', ['async' => true]);
			$async->disconnect();

			if ($response !== 'Call Executed') {
				throw new RuntimeException('async ACALL response mismatch: ' . var_export($response, true));
			}
		} finally {
			cloudrmi_stop_process($proc, $pipes);
		}
	});

	cloudrmi_run_test('acall-absent-with-single-worker', function () {
		[$proc, $pipes, $port] = cloudrmi_start_php_server(null, ['worker_model' => 'single']);
		try {
			cloudrmi_wait_for_server('127.0.0.1', $port);
			$bannerSock = stream_socket_client("tcp://127.0.0.1:{$port}");
			$banner = fgets($bannerSock, 2048);
			fclose($bannerSock);
			if ($banner !== false && strpos($banner, 'ACALL') !== false) {
				throw new RuntimeException('async capability advertised without fork support');
			}

			$sock = stream_socket_client("tcp://127.0.0.1:{$port}");
			fgets($sock, 2048);
			fwrite($sock, "CAPS\r\n");
			$capsLine = fgets($sock, 2048);
			fclose($sock);
			if ($capsLine !== false && strpos($capsLine, 'ACALL') !== false) {
				throw new RuntimeException('CAPS listed ACALL without fork support');
			}

			$client = new CloudRMIClient('127.0.0.1', $port);
			$client->connect();
			$syncResponse = $client->call('TestRmiInterface', 'method', 'sync', ['async' => true]);
			$client->disconnect();

			if ($syncResponse !== 'OK') {
				throw new RuntimeException('async fallback did not return sync result');
			}
		} finally {
			cloudrmi_stop_process($proc, $pipes);
		}
	});
} catch (Throwable $e) {
	$exitCode = 1;
} finally {
	cloudrmi_print_timing_summary();
}

exit($exitCode);
