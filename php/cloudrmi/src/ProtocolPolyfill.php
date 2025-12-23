<?php
/**
 * Polyfill for the cloudrmi/profiweb_cloudrmi PHP extension.
 *
 * Provides a userland implementation of the protocol parsing helpers used by
 * the CloudRMI PHP client when the native extension is unavailable.
 *
 * Protocol version: 2.0.0 (proto v2, lib v2.0.0)
 */
if (!defined('RMI_LIBRARY_VERSION')) define('RMI_LIBRARY_VERSION', '2.0.0-polyfill');

// Status codes
if (!defined('RMI_OK')) define('RMI_OK', 0);
if (!defined('RMI_E_AGAIN')) define('RMI_E_AGAIN', 1);
if (!defined('RMI_E_OVERFLOW')) define('RMI_E_OVERFLOW', 2);
if (!defined('RMI_E_ERR')) define('RMI_E_ERR', 3);

// Error codes
if (!defined('RMI_ERROR_NONE')) define('RMI_ERROR_NONE', 0);
if (!defined('RMI_ERROR_BAD_REQUEST')) define('RMI_ERROR_BAD_REQUEST', 1);
if (!defined('RMI_ERROR_BAD_ARGS')) define('RMI_ERROR_BAD_ARGS', 2);

// Limits
if (!defined('RMI_MAX_LINE')) define('RMI_MAX_LINE', 8192);
if (!defined('RMI_MAX_PAYLOAD')) define('RMI_MAX_PAYLOAD', 10 * 1024 * 1024);
if (!defined('RMI_DEFAULT_MAX_PAYLOAD')) define('RMI_DEFAULT_MAX_PAYLOAD', RMI_MAX_PAYLOAD);
if (!defined('RMI_MAX_BUFFER')) define('RMI_MAX_BUFFER', RMI_MAX_PAYLOAD);
if (!defined('RMI_INLINE_PAYLOAD_THRESHOLD')) define('RMI_INLINE_PAYLOAD_THRESHOLD', (int)(RMI_MAX_LINE - floor(RMI_MAX_LINE * 0.35) - 500));
if (!defined('RMI_MAX_PROTOCOL_VERSION')) define('RMI_MAX_PROTOCOL_VERSION', 2);
if (!defined('CLOUDRMI_RESPONSE_PLAIN')) define('CLOUDRMI_RESPONSE_PLAIN', 0);
if (!defined('CLOUDRMI_RESPONSE_JSON_INLINE')) define('CLOUDRMI_RESPONSE_JSON_INLINE', 1);
if (!defined('CLOUDRMI_RESPONSE_PAYLOAD')) define('CLOUDRMI_RESPONSE_PAYLOAD', 2);

// Events
if (!defined('RMI_EVENT_NONE')) define('RMI_EVENT_NONE', 0);
if (!defined('RMI_EVENT_AUTH')) define('RMI_EVENT_AUTH', 1);
if (!defined('RMI_EVENT_CALL')) define('RMI_EVENT_CALL', 2);
if (!defined('RMI_EVENT_HELP')) define('RMI_EVENT_HELP', 3);
if (!defined('RMI_EVENT_IFACES')) define('RMI_EVENT_IFACES', 4);
if (!defined('RMI_EVENT_RELOAD')) define('RMI_EVENT_RELOAD', 5);
if (!defined('RMI_EVENT_SHUTDOWN')) define('RMI_EVENT_SHUTDOWN', 6);
if (!defined('RMI_EVENT_TIME')) define('RMI_EVENT_TIME', 7);
if (!defined('RMI_EVENT_UPTIME')) define('RMI_EVENT_UPTIME', 8);
if (!defined('RMI_EVENT_NOOP')) define('RMI_EVENT_NOOP', 9);
if (!defined('RMI_EVENT_DEBUG')) define('RMI_EVENT_DEBUG', 10);
if (!defined('RMI_EVENT_QUIT')) define('RMI_EVENT_QUIT', 11);
if (!defined('RMI_EVENT_PROXY')) define('RMI_EVENT_PROXY', 12);
if (!defined('RMI_EVENT_CAPS')) define('RMI_EVENT_CAPS', 13);
if (!defined('RMI_EVENT_SERVER')) define('RMI_EVENT_SERVER', 14);
if (!defined('RMI_EVENT_PROTO')) define('RMI_EVENT_PROTO', 15);
if (!defined('RMI_EVENT_UNKNOWN')) define('RMI_EVENT_UNKNOWN', 16);

// Protocol parser states
if (!defined('RMI_PROTOCOL_IDLE')) define('RMI_PROTOCOL_IDLE', 0);
if (!defined('RMI_PROTOCOL_READING_PAYLOAD')) define('RMI_PROTOCOL_READING_PAYLOAD', 1);
if (!defined('RMI_PROTOCOL_EXECUTE_CALL')) define('RMI_PROTOCOL_EXECUTE_CALL', 2);
if (!defined('RMI_PROTOCOL_SEND_RESPONSE')) define('RMI_PROTOCOL_SEND_RESPONSE', 3);

if (!class_exists('CloudRMIProtocolContext', false)) {
	class CloudRMIProtocolContext
	{
	public string $buffer = '';
	public int $state = RMI_PROTOCOL_IDLE;
	public int $expectedPayloadLen = 0;
	public int $protocolVersion = 1;
	public int $maxPayload = RMI_DEFAULT_MAX_PAYLOAD;
	public int $pendingArgc = 0;
	public string $pendingIface = '';
	public string $pendingMethod = '';
	public string $pendingEncoding = '';

	public function reset(): void
	{
		$version = $this->protocolVersion ?: 1;
		$this->buffer = '';
		$this->state = RMI_PROTOCOL_IDLE;
		$this->expectedPayloadLen = 0;
		$this->pendingArgc = 0;
		$this->pendingIface = '';
		$this->pendingMethod = '';
		$this->pendingEncoding = '';
		$this->protocolVersion = $version;
	}

	public function feed(string $bytes): array
	{
		$len = strlen($bytes);

		if ($len === 0) {
			return $this->status(RMI_OK);
		}

		$this->buffer .= $bytes;

		if ($this->state !== RMI_PROTOCOL_READING_PAYLOAD &&
			strpos($this->buffer, "\n") === false &&
			strlen($this->buffer) > RMI_MAX_LINE) {
			$this->reset();
			return $this->status(RMI_E_OVERFLOW, RMI_ERROR_BAD_REQUEST, 'Line too long');
		}

		return $this->status(RMI_OK);
	}

	public function nextEvent(): array
	{
		while (true) {
			if ($this->state === RMI_PROTOCOL_READING_PAYLOAD) {
				if (strlen($this->buffer) < $this->expectedPayloadLen) {
					return $this->status(RMI_E_AGAIN);
				}

				$payload = ($this->expectedPayloadLen > 0)
					? substr($this->buffer, 0, $this->expectedPayloadLen)
					: '';
				$this->buffer = (string)substr($this->buffer, $this->expectedPayloadLen);

				$event = $this->buildCallEvent($this->pendingIface, $this->pendingMethod, $this->pendingArgc, $this->pendingEncoding, $payload);
				$this->reset();

				return $this->status(RMI_OK, RMI_ERROR_NONE, null, $event);
			}

			if ($this->buffer === '') {
				return $this->status(RMI_E_AGAIN);
			}

			$newlinePos = strpos($this->buffer, "\n");
			if ($newlinePos === false) {
				if (strlen($this->buffer) > RMI_MAX_LINE && $this->state === RMI_PROTOCOL_IDLE) {
					$this->reset();
					return $this->status(RMI_E_OVERFLOW, RMI_ERROR_BAD_REQUEST, 'Line too long');
				}
				return $this->status(RMI_E_AGAIN);
			}

			$line = substr($this->buffer, 0, $newlinePos);
			$this->buffer = (string)substr($this->buffer, $newlinePos + 1);
			$line = rtrim($line, "\r");

			if ($line === '') {
				continue;
			}

			if (strlen($line) > RMI_MAX_LINE) {
				$this->reset();
				return $this->status(RMI_E_OVERFLOW, RMI_ERROR_BAD_REQUEST, 'Line too long');
			}

			[$parsedEvent, $errCode, $errMessage] = $this->parseLine($line);

			// PROTO handling (version negotiation)
			if ($parsedEvent['type'] === RMI_EVENT_PROTO) {
				if ($parsedEvent['arg1'] === '') {
					$this->reset();
					return $this->status(RMI_E_ERR, RMI_ERROR_BAD_ARGS, 'PROTO requires version');
				}

				if (!ctype_digit($parsedEvent['arg1'])) {
					$this->reset();
					return $this->status(RMI_E_ERR, RMI_ERROR_BAD_REQUEST, 'Unsupported protocol version');
				}

				$requested = (int)$parsedEvent['arg1'];
				if ($requested < 1 || $requested > RMI_MAX_PROTOCOL_VERSION) {
					$this->reset();
					return $this->status(RMI_E_ERR, RMI_ERROR_BAD_REQUEST, 'Unsupported protocol version');
				}

				$this->protocolVersion = $requested;
				return $this->status(RMI_OK, $errCode, $errMessage, $parsedEvent);
			}

			if ($parsedEvent['type'] === RMI_EVENT_CALL) {
				if ($parsedEvent['arg1'] === '' || $parsedEvent['arg2'] === '') {
					$this->reset();
					return $this->status(RMI_E_ERR, RMI_ERROR_BAD_ARGS, 'CALL requires iface and method');
				}

				if (strcasecmp($parsedEvent['arg3'], 'payload') === 0) {
					// Payload form only allowed in protocol v2+
					if ($this->protocolVersion < 2) {
						$this->reset();
						return $this->status(RMI_E_ERR, RMI_ERROR_BAD_REQUEST, 'Protocol upgrade required');
					}

					if ($parsedEvent['argc'] < 6 || $parsedEvent['arg5'] === '') {
						$this->reset();
						return $this->status(RMI_E_ERR, RMI_ERROR_BAD_ARGS, 'PAYLOAD length missing');
					}

					if (!ctype_digit($parsedEvent['arg5'])) {
						$this->reset();
						return $this->status(RMI_E_ERR, RMI_ERROR_BAD_REQUEST, 'Invalid payload length');
					}

					$payloadLen = (int)$parsedEvent['arg5'];

					if ($payloadLen < 0 || $payloadLen > $this->maxPayload) {
						$this->reset();
						return $this->status(RMI_E_OVERFLOW, RMI_ERROR_BAD_REQUEST, 'Invalid payload length');
					}

					$this->expectedPayloadLen = $payloadLen;
					$this->pendingIface = $parsedEvent['arg1'];
					$this->pendingMethod = $parsedEvent['arg2'];
					$this->pendingEncoding = $parsedEvent['arg4'];
					$this->pendingArgc = $parsedEvent['argc'];
					$this->state = RMI_PROTOCOL_READING_PAYLOAD;

					if ($payloadLen === 0) {
						$event = $this->buildCallEvent($this->pendingIface, $this->pendingMethod, $this->pendingArgc, $this->pendingEncoding, '');
						$this->reset();
						return $this->status(RMI_OK, RMI_ERROR_NONE, null, $event);
					}

					// Try to consume the payload immediately if already buffered.
					continue;
				}

				if ($parsedEvent['argc'] !== 4 || $parsedEvent['arg3'] === '') {
					$this->reset();
					return $this->status(RMI_E_ERR, RMI_ERROR_BAD_ARGS, 'CALL requires arguments');
				}
			}

			// Unknown commands still return an event with error metadata, mirroring the extension.
			if ($errCode !== RMI_ERROR_NONE) {
				$event = $parsedEvent;
				$this->reset();
				return $this->status(RMI_OK, $errCode, $errMessage, $event);
			}

			return $this->status(RMI_OK, $errCode, $errMessage, $parsedEvent);
		}
	}

	protected function parseLine(string $line): array
	{
		$tokens = preg_split('/\\s+/', trim($line), 6, PREG_SPLIT_NO_EMPTY);
		$tokens = $tokens === false ? [] : $tokens;
		$argc = count($tokens);
		$cmd = $argc > 0 ? strtolower($tokens[0]) : '';

		$event = [
			'type' => RMI_EVENT_NONE,
			'cmd' => $cmd,
			'argc' => $argc,
			'arg1' => $tokens[1] ?? '',
			'arg2' => $tokens[2] ?? '',
			'arg3' => $tokens[3] ?? '',
			'arg4' => $tokens[4] ?? '',
			'arg5' => $tokens[5] ?? '',
			'encoding' => '',
			'payload' => '',
			'payload_len' => 0,
		];

		$errCode = RMI_ERROR_NONE;
		$errMessage = null;

		switch ($cmd) {
			case 'auth':
				$event['type'] = RMI_EVENT_AUTH;
				break;
			case 'call':
				$event['type'] = RMI_EVENT_CALL;
				if ($argc < 3) {
					$errCode = RMI_ERROR_BAD_ARGS;
					$errMessage = 'CALL requires iface and method';
				}
				break;
			case 'help':
				$event['type'] = RMI_EVENT_HELP;
				break;
			case 'ifaces':
				$event['type'] = RMI_EVENT_IFACES;
				break;
			case 'reload':
				$event['type'] = RMI_EVENT_RELOAD;
				break;
			case 'shutdown':
				$event['type'] = RMI_EVENT_SHUTDOWN;
				break;
			case 'time':
				$event['type'] = RMI_EVENT_TIME;
				break;
			case 'uptime':
				$event['type'] = RMI_EVENT_UPTIME;
				break;
			case 'noop':
				$event['type'] = RMI_EVENT_NOOP;
				break;
			case 'debug':
				$event['type'] = RMI_EVENT_DEBUG;
				break;
			case 'quit':
			case 'exit':
				$event['type'] = RMI_EVENT_QUIT;
				break;
			case 'proxy':
				$event['type'] = RMI_EVENT_PROXY;
				break;
			case 'caps':
				$event['type'] = RMI_EVENT_CAPS;
				break;
			case 'server':
				$event['type'] = RMI_EVENT_SERVER;
				break;
			case 'proto':
				$event['type'] = RMI_EVENT_PROTO;
				break;
			case '':
				$event['type'] = RMI_EVENT_UNKNOWN;
				$errCode = RMI_ERROR_BAD_REQUEST;
				$errMessage = 'Empty command';
				break;
			default:
				$event['type'] = RMI_EVENT_UNKNOWN;
				$errCode = RMI_ERROR_BAD_REQUEST;
				$errMessage = 'Unknown command';
				break;
		}

		return [$event, $errCode, $errMessage];
	}

	protected function buildCallEvent(string $iface, string $method, int $argc, string $encoding, string $payload): array
	{
		return [
			'type' => RMI_EVENT_CALL,
			'cmd' => 'call',
			'argc' => $argc,
			'arg1' => $iface,
			'arg2' => $method,
			'arg3' => '',
			'arg4' => '',
			'arg5' => '',
			'encoding' => $encoding,
			'payload' => $payload,
			'payload_len' => strlen($payload),
		];
	}

	protected function status(int $status, int $errCode = RMI_ERROR_NONE, ?string $errMessage = null, ?array $event = null): array
	{
		return [
			'status' => $status,
			'err_code' => $errCode,
			'err_message' => $errMessage,
			'event' => $event,
		];
	}
	}
}

/**
 * Userland replacements for the native extension functions.
 */
if (!function_exists('cloudrmi_protocol_create')) {
	if (!defined('RMI_USES_POLYFILL')) define('RMI_USES_POLYFILL', true);

	function cloudrmi_protocol_create()
	{
		return new CloudRMIProtocolContext();
	}
}

if (!function_exists('cloudrmi_protocol_destroy')) {
	function cloudrmi_protocol_destroy($ctx): void
	{
		if ($ctx instanceof CloudRMIProtocolContext) {
			// noop; allow GC to reclaim object.
		}
	}
}

if (!function_exists('cloudrmi_protocol_reset')) {
	function cloudrmi_protocol_reset($ctx): void
	{
		if ($ctx instanceof CloudRMIProtocolContext) {
			$ctx->reset();
		}
	}
}

if (!function_exists('cloudrmi_protocol_feed')) {
	function cloudrmi_protocol_feed($ctx, string $bytes): array
	{
		if (!($ctx instanceof CloudRMIProtocolContext)) {
			return [
				'status' => RMI_E_ERR,
				'err_code' => RMI_ERROR_BAD_REQUEST,
				'err_message' => 'Invalid arguments',
				'event' => null,
			];
		}

		return $ctx->feed($bytes);
	}
}

if (!function_exists('cloudrmi_protocol_next_event')) {
	function cloudrmi_protocol_next_event($ctx): array
	{
		if (!($ctx instanceof CloudRMIProtocolContext)) {
			return [
				'status' => RMI_E_ERR,
				'err_code' => RMI_ERROR_BAD_REQUEST,
				'err_message' => 'Invalid arguments',
				'event' => null,
			];
		}

		return $ctx->nextEvent();
	}
}

if (!function_exists('cloudrmi_protocol_set_version')) {
	function cloudrmi_protocol_set_version($ctx, int $version): bool
	{
		if (!($ctx instanceof CloudRMIProtocolContext)) {
			return false;
		}

		if ($version < 1 || $version > RMI_MAX_PROTOCOL_VERSION) {
			return false;
		}

		$ctx->protocolVersion = $version;
		return true;
	}
}

if (!function_exists('cloudrmi_protocol_set_max_payload')) {
	function cloudrmi_protocol_set_max_payload($ctx, int $maxPayload): bool
	{
		if (!($ctx instanceof CloudRMIProtocolContext) || $maxPayload <= 0) {
			return false;
		}

		$ctx->maxPayload = $maxPayload;
		return true;
	}
}

if (!function_exists('cloudrmi_protocol_validate_line')) {
	function cloudrmi_protocol_validate_line(string $line): bool
	{
		if (strlen($line) > RMI_MAX_LINE) {
			return false;
		}

		return strpos($line, "\n") === false && strpos($line, "\r") === false;
	}
}

if (!function_exists('cloudrmi_parse_banner')) {
	function cloudrmi_parse_banner(string $line)
	{
		if (!cloudrmi_protocol_validate_line($line)) {
			return false;
		}

		$line = ltrim($line);

		if (!preg_match('/^220\s+([A-Za-z0-9._-]+)(?:\s+(.*))?$/', $line, $matches)) {
			return false;
		}

		$context = $matches[1];
		$rest = $matches[2] ?? '';

		// No EXT  - legacy v1
		if (!preg_match('/(?:^|\s)EXT(?:\s+(.*))?$/i', $rest, $extMatch)) {
			return [
				'status' => 220,
				'context' => $context,
				'advertised_max_protocol' => 1,
				'capabilities' => [],
			];
		}

		$extTail = trim($extMatch[1] ?? '');

		// EXT present but empty → invalid
		if ($extTail === '') {
			return false;
		}

		$version = 1;
		$caps = [];

		$tokens = preg_split('/\s*,\s*/', $extTail);
		foreach ($tokens as $token) {
			$token = trim($token);
			if ($token === '') {
				return false;
			}

			// PROTO token
			if (stripos($token, 'PROTO=') === 0) {
				$protoVal = substr($token, strlen('PROTO='));
				if (!ctype_digit($protoVal)) {
					return false;
				}
				$version = (int)$protoVal;
				if ($version < 1 || $version > RMI_MAX_PROTOCOL_VERSION) {
					return false;
				}
				continue;
			}

			// Capability token
			$cap = strtoupper($token);
			if (preg_match('/[^A-Z0-9_-]/', $cap)) {
				return false;
			}
			$caps[] = $cap;
		}

		return [
			'status' => 220,
			'context' => $context,
			'advertised_max_protocol' => $version,
			'capabilities' => $caps,
		];
	}
}

if (!function_exists('cloudrmi_parse_response_line')) {
	function cloudrmi_parse_response_line(string $line)
	{
		if (!cloudrmi_protocol_validate_line($line)) {
			return false;
		}

		$cursor = ltrim($line);
		if (!preg_match('/^([0-9]+)/', $cursor, $statusMatch)) {
			return false;
		}

		$status = (int)$statusMatch[1];
		$cursor = ltrim(substr($cursor, strlen($statusMatch[1])));

		if ($cursor === '') {
			return [
				'status' => $status,
				'kind' => CLOUDRMI_RESPONSE_PLAIN,
				'encoding' => '',
				'payload_len' => 0,
				'body' => '',
			];
		}

		if (stripos($cursor, 'JSON') === 0 && (strlen($cursor) === 4 || ctype_space($cursor[4]))) {
			$body = ltrim(substr($cursor, 4));
			return [
				'status' => $status,
				'kind' => CLOUDRMI_RESPONSE_JSON_INLINE,
				'encoding' => '',
				'payload_len' => 0,
				'body' => $body,
			];
		}

		if (stripos($cursor, 'PAYLOAD') === 0 && (strlen($cursor) === 7 || ctype_space($cursor[7]))) {
			$cursor = ltrim(substr($cursor, 7));
			$parts = preg_split('/\\s+/', $cursor);
			if (count($parts) < 2) {
				return false;
			}

			$encoding = $parts[0];
			if (!ctype_digit($parts[1])) {
				return false;
			}

			$len = (int)$parts[1];
			return [
				'status' => $status,
				'kind' => CLOUDRMI_RESPONSE_PAYLOAD,
				'encoding' => $encoding,
				'payload_len' => $len,
				'body' => '',
			];
		}

		return [
			'status' => $status,
			'kind' => CLOUDRMI_RESPONSE_PLAIN,
			'encoding' => '',
			'payload_len' => 0,
			'body' => $cursor,
		];
	}
}

if (!function_exists('cloudrmi_parse_enum_response')) {
	function cloudrmi_parse_enum_response(string $line)
	{
		$parsed = cloudrmi_parse_response_line($line);
		if (!is_array($parsed) || $parsed['status'] !== 200 || $parsed['kind'] !== CLOUDRMI_RESPONSE_JSON_INLINE) {
			return false;
		}

		$json = trim($parsed['body']);
		$decoded = json_decode($json, true);
		if (!is_array($decoded)) {
			return false;
		}

		$result = [];
		foreach ($decoded as $value) {
			if (!is_string($value)) {
				return false;
			}
			$token = strtoupper(trim($value));
			if ($token === '') {
				return false;
			}
			$result[] = $token;
		}

		return $result;
	}
}

if (!function_exists('cloudrmi_protocol_payload_allowed')) {
	function cloudrmi_protocol_payload_allowed($ctx, int $payloadLen): bool
	{
		if (!($ctx instanceof CloudRMIProtocolContext) || $payloadLen < 0) {
			return false;
		}

		return $payloadLen <= $ctx->maxPayload;
	}
}
