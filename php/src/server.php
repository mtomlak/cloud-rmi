<?php
if (!extension_loaded('cloudrmi')) {
    fwrite(STDERR, "cloudrmi extension is required\n");
    exit(1);
}

$options = getopt('', ['port:', 'address::']);
$address = $options['address'] ?? '127.0.0.1';
$port = isset($options['port']) ? (int)$options['port'] : 9501;

$server = @stream_socket_server("tcp://{$address}:{$port}", $errno, $errstr);
if ($server === false) {
    fwrite(STDERR, "Failed to start server: {$errstr}\n");
    exit(1);
}

stream_set_blocking($server, true);
fwrite(STDOUT, "listening on {$address}:{$port}\n");

while ($client = @stream_socket_accept($server, -1)) {
    stream_set_blocking($client, true);
    $ctx = cloudrmi_protocol_create();
    if ($ctx === false) {
        fwrite($client, "500 context creation failed\n");
        fclose($client);
        continue;
    }

    while (!feof($client)) {
        $data = fread($client, 4096);
        if ($data === '' || $data === false) {
            break;
        }

        $feedResult = cloudrmi_protocol_feed($ctx, $data);
        if ($feedResult['status'] !== RMI_OK) {
            $message = $feedResult['err_message'] ?? 'feed error';
            fwrite($client, "400 {$message}\n");
            break;
        }

        while (true) {
            $eventResult = cloudrmi_protocol_next_event($ctx);
            if ($eventResult['status'] === RMI_E_AGAIN) {
                break;
            }

            if ($eventResult['status'] !== RMI_OK || $eventResult['event'] === null) {
                $message = $eventResult['err_message'] ?? 'parse error';
                fwrite($client, "400 {$message}\n");
                break 2;
            }

            $event = $eventResult['event'];
            if ((int)$event['type'] !== RMI_EVENT_CALL) {
                fwrite($client, "400 unexpected command\n");
                break 2;
            }

            if ((int)$event['argc'] >= 4) {
                $line = sprintf(
                    "CALL %s %s %s\n",
                    $event['arg1'],
                    $event['arg2'],
                    $event['arg3']
                );
                fwrite($client, $line);
            } else {
                $encoding = $event['encoding'] !== '' ? $event['encoding'] : 'NONE';
                fwrite($client, sprintf("CALL %s %s\n", $event['arg1'], $event['arg2']));
                fwrite($client, sprintf("PAYLOAD %s %d\n", $encoding, $event['payload_len']));
                if ($event['payload_len'] > 0) {
                    fwrite($client, $event['payload']);
                }
            }
        }
    }

    fclose($client);
}
