<?php
require_once __DIR__ . '/../../src/client.php';

function assert_true($condition, string $message): void
{
    if (!$condition) {
        fwrite(STDERR, "Assertion failed: {$message}\n");
        exit(1);
    }
}

function assert_equals($expected, $actual, string $message): void
{
    if ($expected !== $actual) {
        fwrite(STDERR, "Assertion failed: {$message} (expected " . var_export($expected, true) . ", got " . var_export($actual, true) . ")\n");
        exit(1);
    }
}

$ctx = cloudrmi_protocol_create();
assert_true($ctx !== false, 'create context');
assert_true(cloudrmi_protocol_set_version($ctx, 2) === true, 'set protocol version to 2');

// legacy CALL parses
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

cloudrmi_protocol_reset($ctx);

// CALL + PAYLOAD parses
$payload = "bin\0ary";
$bytes = "CALL iface method\nPAYLOAD BLOB " . strlen($payload) . "\n" . $payload;
$feed = cloudrmi_protocol_feed($ctx, $bytes);
assert_equals(RMI_OK, $feed['status'], 'feed payload call');
$eventRes = cloudrmi_protocol_next_event($ctx);
assert_equals(RMI_OK, $eventRes['status'], 'payload event status');
$event = $eventRes['event'];
assert_equals(RMI_EVENT_CALL, $event['type'], 'payload event type');
assert_equals(3, $event['argc'], 'payload argc');
assert_equals('BLOB', $event['encoding'], 'payload encoding');
assert_equals($payload, $event['payload'], 'payload bytes');
assert_equals(strlen($payload), $event['payload_len'], 'payload len');

cloudrmi_protocol_reset($ctx);

// PAYLOAD without CALL errors
$feed = cloudrmi_protocol_feed($ctx, "PAYLOAD PLAIN 3\nabc");
assert_equals(RMI_OK, $feed['status'], 'feed ok even when error later');
$eventRes = cloudrmi_protocol_next_event($ctx);
assert_equals(RMI_E_ERR, $eventRes['status'], 'payload without call status');
assert_equals(RMI_ERROR_BAD_REQUEST, $eventRes['err_code'], 'payload without call error code');
assert_true($eventRes['event'] === null, 'no event on error');

cloudrmi_protocol_reset($ctx);

// CALL inline args then PAYLOAD should error on payload
$feed = cloudrmi_protocol_feed($ctx, "CALL iface method inline\nPAYLOAD PLAIN 1\na");
assert_equals(RMI_OK, $feed['status'], 'feed mixed payload');
$eventRes = cloudrmi_protocol_next_event($ctx);
assert_equals(RMI_OK, $eventRes['status'], 'first event ok');
$eventRes = cloudrmi_protocol_next_event($ctx);
assert_equals(RMI_E_ERR, $eventRes['status'], 'payload after inline call errors');
assert_equals(RMI_ERROR_BAD_REQUEST, $eventRes['err_code'], 'payload after inline call err code');

cloudrmi_protocol_reset($ctx);

// partial payload returns AGAIN
$feed = cloudrmi_protocol_feed($ctx, "CALL iface method\nPAYLOAD RAW 5\nabc");
assert_equals(RMI_OK, $feed['status'], 'partial feed ok');
$eventRes = cloudrmi_protocol_next_event($ctx);
assert_equals(RMI_E_AGAIN, $eventRes['status'], 'partial payload again');
$more = cloudrmi_protocol_feed($ctx, "de");
assert_equals(RMI_OK, $more['status'], 'second feed ok');
$eventRes = cloudrmi_protocol_next_event($ctx);
assert_equals(RMI_OK, $eventRes['status'], 'payload complete after more data');
assert_equals('abcde', $eventRes['event']['payload'], 'payload assembled');

fwrite(STDOUT, "All unit protocol tests passed\n");
