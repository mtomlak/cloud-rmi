<?php
// Protocol-consumer harness for CloudRMI events.
// Not a production server; validates CloudRMI parsing and event handling.
// Consumes CALL events and responds with status lines only.
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

    $fatal = false;
    $errorSent = false;
    $okPayload = base64_encode('OK');

    while (true) {
        $data = fread($client, 4096);
        if ($data === '' || $data === false) {
            break;
        }

        $feedResult = cloudrmi_protocol_feed($ctx, $data);
        if ($feedResult['status'] !== RMI_OK || $feedResult['err_code'] !== RMI_ERROR_NONE) {
            if (!$errorSent) {
                $message = $feedResult['err_message'] ?? 'feed error';
                fwrite($client, "400 {$message}\n");
                $errorSent = true;
            }
            $fatal = true;
            break;
        }

        while (true) {
            $eventResult = cloudrmi_protocol_next_event($ctx);
            if ($eventResult['status'] === RMI_E_AGAIN) {
                break;
            }

            if ($eventResult['status'] !== RMI_OK || $eventResult['event'] === null || $eventResult['err_code'] !== RMI_ERROR_NONE) {
                if (!$errorSent) {
                    $message = $eventResult['err_message'] ?? 'parse error';
                    fwrite($client, "400 {$message}\n");
                    $errorSent = true;
                }
                $fatal = true;
                break 2;
            }

            $event = $eventResult['event'];
            if ((int)$event['type'] !== RMI_EVENT_CALL) {
                fwrite($client, "400 unexpected command\n");
                $fatal = true;
                $errorSent = true;
                break 2;
            }

            $isLegacyCall = ((int)$event['argc'] >= 4);
            if ($isLegacyCall) {
                if ((int)$event['payload_len'] !== 0 || $event['payload'] !== '') {
                    fwrite($client, "400 invalid CALL payload mix\n");
                    $fatal = true;
                    $errorSent = true;
                    break 2;
                }

                $decodedArg = base64_decode($event['arg3'], true);
                if ($decodedArg === false) {
                    fwrite($client, "400 invalid base64 args\n");
                    $fatal = true;
                    $errorSent = true;
                    break 2;
                }
            } else {
                $payload = (string)$event['payload'];
                if ((int)$event['payload_len'] !== strlen($payload)) {
                    fwrite($client, "400 payload length mismatch\n");
                    $fatal = true;
                    $errorSent = true;
                    break 2;
                }

                if ((string)$event['arg3'] !== '') {
                    fwrite($client, "400 invalid CALL arguments\n");
                    $fatal = true;
                    $errorSent = true;
                    break 2;
                }
            }

            fwrite($client, "200 {$okPayload}\n");
        }

        if ($fatal) {
            break;
        }
    }

    cloudrmi_protocol_destroy($ctx);
    fclose($client);
}
