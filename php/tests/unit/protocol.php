<?php
require_once __DIR__ . '/../support/test_harness.php';

function assert_true($condition, string $message): void
{
	if (!$condition) {
		throw new RuntimeException("Assertion failed: {$message}");
	}
}

function assert_equals($expected, $actual, string $message): void
{
	if ($expected !== $actual) {
		throw new RuntimeException(
			"Assertion failed: {$message} (expected "
			. var_export($expected, true)
			. ", got "
			. var_export($actual, true)
			. ")"
		);
	}
}

cloudrmi_set_suite('unit-protocol');

$ctx = cloudrmi_protocol_create();
$exitCode = 0;

try {
	assert_true($ctx !== false, 'create context');
	assert_true(cloudrmi_protocol_set_max_payload($ctx, 1024 * 1024), 'set max payload');
	assert_true(cloudrmi_protocol_set_version($ctx, 2), 'set protocol version');

	cloudrmi_run_test('legacy-call-parses', function () use ($ctx) {
		$feed = cloudrmi_protocol_feed($ctx, "CALL iface method args\n");
		assert_equals(RMI_OK, $feed['status'], 'feed status OK');
		$eventRes = cloudrmi_protocol_next_event($ctx);
		assert_equals(RMI_OK, $eventRes['status'], 'event status OK');
		assert_true($eventRes['event'] !== null, 'event exists');
		$event = $eventRes['event'];
		assert_equals(RMI_EVENT_CALL, $event['type'], 'event type call');
		assert_equals(4, $event['argc'], 'argc legacy');
		assert_equals('args', $event['arg3'], 'inline args');
		assert_equals('', $event['payload'], 'no payload string');
		assert_equals(0, $event['payload_len'], 'no payload length');
	});

	cloudrmi_run_test('proto-upgrade-event', function () use ($ctx) {
		cloudrmi_protocol_reset($ctx);
		$upgrade = cloudrmi_protocol_feed($ctx, "PROTO 2\n");
		assert_equals(RMI_OK, $upgrade['status'], 'proto upgrade feed ok');
		$upgradeEvent = cloudrmi_protocol_next_event($ctx);
		assert_equals(RMI_OK, $upgradeEvent['status'], 'proto upgrade event status');
		assert_true($upgradeEvent['event'] !== null, 'proto upgrade event exists');
		assert_equals('proto', $upgradeEvent['event']['cmd'], 'proto event type');
	});

	cloudrmi_run_test('payload-call-parses', function () use ($ctx) {
		cloudrmi_protocol_reset($ctx);
		$payload = "bin\0ary";
		$bytes = "CALL iface method PAYLOAD BLOB " . strlen($payload) . "\n" . $payload;
		$feed = cloudrmi_protocol_feed($ctx, $bytes);
		assert_equals(RMI_OK, $feed['status'], 'feed payload call');
		$eventRes = cloudrmi_protocol_next_event($ctx);
		assert_equals(RMI_OK, $eventRes['status'], 'payload event status');
		$event = $eventRes['event'];
		assert_equals(RMI_EVENT_CALL, $event['type'], 'payload event type');
		assert_equals(6, $event['argc'], 'payload argc');
		assert_equals('BLOB', $event['encoding'], 'payload encoding');
		assert_equals($payload, $event['payload'], 'payload bytes');
		assert_equals(strlen($payload), $event['payload_len'], 'payload len');
	});

	cloudrmi_run_test('payload-without-call-errors', function () use ($ctx) {
		cloudrmi_protocol_reset($ctx);
		$feed = cloudrmi_protocol_feed($ctx, "PAYLOAD PLAIN 3\nabc");
		assert_equals(RMI_OK, $feed['status'], 'feed ok even when error later');
		$eventRes = cloudrmi_protocol_next_event($ctx);
		assert_equals(RMI_OK, $eventRes['status'], 'payload without call status');
		assert_equals(RMI_ERROR_BAD_REQUEST, $eventRes['err_code'], 'payload without call error code');
		assert_true($eventRes['event'] !== null && $eventRes['event']['type'] === RMI_EVENT_UNKNOWN, 'unknown event on stray payload');
	});

	cloudrmi_run_test('inline-then-payload-errors', function () use ($ctx) {
		cloudrmi_protocol_reset($ctx);
		$feed = cloudrmi_protocol_feed($ctx, "CALL iface method inline\nPAYLOAD PLAIN 1\na");
		assert_equals(RMI_OK, $feed['status'], 'feed mixed payload');
		$eventRes = cloudrmi_protocol_next_event($ctx);
		assert_equals(RMI_OK, $eventRes['status'], 'first event ok');
		$eventRes = cloudrmi_protocol_next_event($ctx);
		assert_equals(RMI_OK, $eventRes['status'], 'payload after inline call errors');
		assert_equals(RMI_ERROR_BAD_REQUEST, $eventRes['err_code'], 'payload after inline call err code');
		assert_true($eventRes['event']['type'] === RMI_EVENT_UNKNOWN, 'unknown payload command after inline call');
	});

	cloudrmi_run_test('partial-payload-requires-more', function () use ($ctx) {
		cloudrmi_protocol_reset($ctx);
		$feed = cloudrmi_protocol_feed($ctx, "CALL iface method PAYLOAD RAW 5\nabc");
		assert_equals(RMI_OK, $feed['status'], 'partial feed ok');
		$eventRes = cloudrmi_protocol_next_event($ctx);
		assert_equals(RMI_E_AGAIN, $eventRes['status'], 'partial payload again');
		$more = cloudrmi_protocol_feed($ctx, "de");
		assert_equals(RMI_OK, $more['status'], 'second feed ok');
		$eventRes = cloudrmi_protocol_next_event($ctx);
		assert_equals(RMI_OK, $eventRes['status'], 'payload complete after more data');
		assert_equals('abcde', $eventRes['event']['payload'], 'payload assembled');
	});

	cloudrmi_run_test('max-line-boundaries', function () use ($ctx) {
		cloudrmi_protocol_reset($ctx);
		$bufSize = defined('RMI_BUF_SZ') ? RMI_BUF_SZ : 8192;
		$long_line = str_repeat('a', $bufSize + 1);
		$status = cloudrmi_protocol_feed($ctx, $long_line);
		assert_equals(RMI_E_OVERFLOW, $status['status'] ?? $status, 'overflow status');
		$again = cloudrmi_protocol_next_event($ctx);
		assert_equals(RMI_E_AGAIN, $again['status'], 'again after overflow');

		cloudrmi_protocol_reset($ctx);
		$double_ping = "PING\nPING\n";
		$feed = cloudrmi_protocol_feed($ctx, $double_ping);
		assert_equals(RMI_OK, $feed['status'], 'double ping feed ok');
		assert_equals(RMI_OK, cloudrmi_protocol_next_event($ctx)['status'], 'first ping event');
		assert_equals(RMI_OK, cloudrmi_protocol_next_event($ctx)['status'], 'second ping event');
	});

	cloudrmi_run_test('max-payload-boundaries', function () use ($ctx) {
		cloudrmi_protocol_reset($ctx);
		cloudrmi_protocol_set_max_payload($ctx, 16);
		$proto = cloudrmi_protocol_feed($ctx, "PROTO 2\n");
		assert_equals(RMI_OK, $proto['status'], 'proto feed ok');
		cloudrmi_protocol_next_event($ctx);
		$header = "CALL demo method PAYLOAD RAW 16\n";
		cloudrmi_protocol_feed($ctx, $header);
		$status = cloudrmi_protocol_next_event($ctx);
		assert_equals(RMI_E_AGAIN, $status['status'], 'await payload body');
		$payload = str_repeat('x', 16);
		cloudrmi_protocol_feed($ctx, $payload);
		$eventRes = cloudrmi_protocol_next_event($ctx);
		assert_equals(RMI_OK, $eventRes['status'], 'payload event after cap');
		assert_equals(16, $eventRes['event']['payload_len'], 'payload length cap');

		cloudrmi_protocol_reset($ctx);
		cloudrmi_protocol_set_max_payload($ctx, 8);
		cloudrmi_protocol_set_version($ctx, 2);
		$over_header = "CALL demo method PAYLOAD RAW 9\n";
		$overflow = cloudrmi_protocol_feed($ctx, $over_header);
		assert_equals(RMI_E_OVERFLOW, $overflow['status'] ?? $overflow, 'overflow beyond max payload');
	});

	cloudrmi_run_test('protocol-upgrade-requirements', function () use (&$ctx) {
		cloudrmi_protocol_destroy($ctx);
		$ctx = cloudrmi_protocol_create();
		assert_true($ctx !== false, 'recreate context');
		$bytes = "CALL demo method PAYLOAD RAW 3\nabc";
		$status = cloudrmi_protocol_feed($ctx, $bytes);
		$event = cloudrmi_protocol_next_event($ctx);
		assert_true(
			($status['status'] ?? $status) === RMI_E_ERR || $event['err_code'] === RMI_ERROR_BAD_REQUEST,
			'payload without proto errors'
		);

		cloudrmi_protocol_destroy($ctx);
		$ctx = cloudrmi_protocol_create();
		assert_true($ctx !== false, 'recreate context again');
		$bytesV2 = "PROTO 2\nCALL demo method PAYLOAD RAW 3\nbar";
		cloudrmi_protocol_feed($ctx, $bytesV2);
		cloudrmi_protocol_next_event($ctx);
		$eventRes = cloudrmi_protocol_next_event($ctx);
		assert_equals(RMI_OK, $eventRes['status'], 'v2 payload event after proto');
		assert_equals('bar', $eventRes['event']['payload'], 'payload bytes kept');

		cloudrmi_protocol_destroy($ctx);
		$ctx = cloudrmi_protocol_create();
		assert_true($ctx !== false, 'recreate context for legacy payload rejection');
		$proto_and_payload = "PROTO 2\nCALL demo method\nPAYLOAD RAW 3\nbar";
		cloudrmi_protocol_feed($ctx, $proto_and_payload);
		cloudrmi_protocol_next_event($ctx);
		$status = cloudrmi_protocol_next_event($ctx);
		assert_equals(RMI_E_ERR, $status['status'] ?? $status, 'legacy payload framing rejected');
	});
} catch (Throwable $e) {
	$exitCode = 1;
} finally {
	if ($ctx) {
		cloudrmi_protocol_destroy($ctx);
	}
	cloudrmi_print_timing_summary();
}

exit($exitCode);
