<?php
require_once __DIR__ . '/../support/test_harness.php';

use ProfiWeb\CloudRMI\CloudRMIClient;

cloudrmi_set_suite('unix_socket_client');

$socketPath = sys_get_temp_dir() . '/cloudrmi-' . uniqid('', true) . '.sock';
[$proc, $pipes, $port] = cloudrmi_start_php_server(null, ['unix_socket' => $socketPath]);
$exitCode = 0;

try {
	cloudrmi_wait_for_unix_socket($socketPath);

	cloudrmi_run_test('banner-and-downgrade-rejection', function () use ($socketPath) {
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
	});

	cloudrmi_run_test('legacy-and-payload-calls', function () use ($socketPath) {
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
	});
} catch (Throwable $e) {
	$exitCode = 1;
} finally {
	cloudrmi_stop_process($proc, $pipes);
	@unlink($socketPath);
	cloudrmi_print_timing_summary();
}

exit($exitCode);
