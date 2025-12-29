<?php
require_once __DIR__ . '/../support/test_harness.php';

use ProfiWeb\CloudRMI\CloudRMIClient;

function decode_inline_response(string $raw): mixed
{
	$payload = $raw;
	$encoding = 'PLAIN';

	if (strpos($raw, ':') !== false) {
		[$encoding, $payload] = explode(':', $raw, 2);
	}

	$encoding = strtoupper($encoding);

	switch ($encoding) {
		case 'PHP':
			$decoded = unserialize(base64_decode($payload));
			break;

		case 'JSON':
			$decoded = json_decode(base64_decode($payload), true);
			break;

		case 'RAW':
			$decoded = base64_decode($payload);
			break;

		case 'PLAIN':
			$decoded = base64_decode($payload, true);
			if ($decoded === false) {
				$decoded = $payload;
			}
			break;

		default:
			$decoded = $payload;
			break;
	}

	return $decoded;
}

function decode_payload_response(string $encoding, string $payload): mixed
{
	$encoding = strtoupper($encoding);

	switch ($encoding) {
		case 'PHP':
			return unserialize($payload);

		case 'JSON':
			return json_decode($payload, true);

		case 'RAW':
		case 'PLAIN':
			return $payload;

		default:
			return $payload;
	}
}

function read_response_body($stream, string $line): mixed
{
	$trimmed = trim($line);
	if (!preg_match('/^\d{3}\s+(.+)$/', $trimmed, $matches)) {
		throw new RuntimeException('malformed response line: ' . $trimmed);
	}

	$rest = $matches[1];
	if (preg_match('/^PAYLOAD\s+(\S+)\s+(\d+)$/i', $rest, $payloadMatches)) {
		$length = (int) $payloadMatches[2];
		$body = ($length > 0) ? stream_get_contents($stream, $length) : '';
		if ($body === false || strlen($body) !== $length) {
			throw new RuntimeException('payload response body truncated');
		}
		return decode_payload_response($payloadMatches[1], $body);
	}

	return decode_inline_response($rest);
}

cloudrmi_set_suite('protocol_negotiation');

$exitCode = 0;

try {
	cloudrmi_run_test('proto2-negotiation-and-payload', function () {
		[$proc, $pipes, $port] = cloudrmi_start_php_server(cloudrmi_random_port(15000, 500), [
			'proto_max' => 2,
			'worker_model' => cloudrmi_worker_model(),
		]);
		try {
			cloudrmi_wait_for_server('127.0.0.1', $port);
			$raw = stream_socket_client("tcp://127.0.0.1:{$port}");
			fgets($raw, 2048);
			fwrite($raw, "PROTO 2\r\n");
			$protoLine = fgets($raw, 2048);
			if ($protoLine === false || strpos($protoLine, '200 PROTO 2') === false) {
				throw new RuntimeException('v2 proto negotiation failed');
			}

			fwrite($raw, "CALL TestRmiInterface upload PAYLOAD RAW 3\r\nabc");
			$responseLine = fgets($raw, 2048);
			if ($responseLine === false || strpos($responseLine, '200 ') !== 0) {
				throw new RuntimeException('payload call failed: ' . var_export($responseLine, true));
			}
			$payload = read_response_body($raw, $responseLine);
			fclose($raw);
			if ($payload !== 'OK') {
				throw new RuntimeException('payload response body mismatch: ' . var_export($payload, true));
			}

			$clientV2 = new CloudRMIClient('127.0.0.1', $port);
			$clientV2->connect();
			$clientResult = $clientV2->call('TestRmiInterface', 'method', 'hello');
			if ($clientResult !== 'OK') {
				throw new RuntimeException('client inline call failed for v2');
			}

			$binary = "bin\0data";
			$payloadResult = $clientV2->call('TestRmiInterface', 'upload', $binary, ['payload' => true, 'encoding' => 'RAW']);
			$clientV2->disconnect();
			if ($payloadResult !== 'OK') {
				throw new RuntimeException('client payload call failed for v2');
			}
		} finally {
			cloudrmi_stop_process($proc, $pipes);
		}
	});

	cloudrmi_run_test('proto1-clamps-upgrade', function () {
		[$proc, $pipes, $port] = cloudrmi_start_php_server(cloudrmi_random_port(16000, 500), [
			'proto_max' => 1,
			'worker_model' => cloudrmi_worker_model(),
		]);
		try {
			cloudrmi_wait_for_server('127.0.0.1', $port);
			$sock = stream_socket_client("tcp://127.0.0.1:{$port}");
			fgets($sock, 2048);
			fwrite($sock, "PROTO 2\r\n");
			$protoLine = fgets($sock, 2048);
			if ($protoLine === false || strpos($protoLine, '200 PROTO 1') === false) {
				throw new RuntimeException('v1 server did not clamp proto negotiation');
			}
			fclose($sock);

			$clientV1 = new CloudRMIClient('127.0.0.1', $port);
			$clientV1->connect();
			$legacy = $clientV1->call('TestRmiInterface', 'method', 'legacy');
			$clientV1->disconnect();
			if ($legacy !== 'OK') {
				throw new RuntimeException('legacy client call failed on v1 server');
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
