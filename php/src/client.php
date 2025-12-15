<?php
if (!extension_loaded('cloudrmi')) {
    fwrite(STDERR, "cloudrmi extension is required\n");
    exit(1);
}

function cloudrmi_connect(string $host, int $port)
{
    $client = @stream_socket_client("tcp://{$host}:{$port}", $errno, $errstr, 2);
    if ($client === false) {
        throw new RuntimeException("connect failed: {$errstr}");
    }
    stream_set_blocking($client, true);
    stream_set_timeout($client, 1);
    return $client;
}

function cloudrmi_send_legacy_call($socket, string $iface, string $method, string $args): void
{
    $line = sprintf("CALL %s %s %s\n", $iface, $method, $args);
    fwrite($socket, $line);
}

function cloudrmi_send_payload_call($socket, string $iface, string $method, string $encoding, string $payload): void
{
    $header = sprintf("CALL %s %s\nPAYLOAD %s %d\n", $iface, $method, $encoding, strlen($payload));
    fwrite($socket, $header);
    if ($payload !== '') {
        fwrite($socket, $payload);
    }
}

function cloudrmi_read_events($socket, $ctx, float $timeoutSeconds = 2.0): array
{
    $start = microtime(true);
    $events = [];
    while (microtime(true) - $start < $timeoutSeconds) {
        $chunk = fread($socket, 4096);
        $info = stream_get_meta_data($socket);

        if ($chunk === '' || $chunk === false) {
            if (!empty($info['timed_out'])) {
                if (!empty($events)) {
                    break;
                }
                continue;
            }
            break;
        }

        $feed = cloudrmi_protocol_feed($ctx, $chunk);
        if ($feed['status'] !== RMI_OK) {
            throw new RuntimeException($feed['err_message'] ?? 'feed error');
        }

        while (true) {
            $next = cloudrmi_protocol_next_event($ctx);
            if ($next['status'] === RMI_E_AGAIN) {
                break;
            }
            if ($next['status'] !== RMI_OK || $next['event'] === null) {
                throw new RuntimeException($next['err_message'] ?? 'parse error');
            }
            $events[] = $next['event'];
        }

        if (!empty($info['timed_out']) && !empty($events)) {
            break;
        }
    }

    return $events;
}
