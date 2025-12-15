<?php

namespace RemoteMethodInvocation;

/**
 * Minimal server wrapper that delegates protocol parsing to libcloudrmi.
 * This is not intended to be a production server replacement; it only
 * serves as a thin compatibility harness for tests.
 */
class SimpleRMIServer
{
    /** @var resource */
    private $server;

    public function __construct(string $address, int $port)
    {
        if (!extension_loaded('cloudrmi')) {
            throw new RemoteMethodInvocationException('cloudrmi extension is required for SimpleRMIServer');
        }

        $this->server = @stream_socket_server("tcp://{$address}:{$port}", $errno, $errstr);
        if ($this->server === false) {
            throw new RemoteMethodInvocationException("Failed to bind server: {$errstr}");
        }
        stream_set_blocking($this->server, false);
    }

    /**
     * Accept a single connection and process events until the socket closes.
     * Responses use legacy status line semantics.
     */
    public function serve(callable $handler): void
    {
        while ($client = @stream_socket_accept($this->server, 1)) {
            $ctx = function_exists('cloudrmi_protocol_create') ? cloudrmi_protocol_create() : null;
            if ($ctx === null) {
                fwrite($client, "500 protocol extension missing\n");
                fclose($client);
                continue;
            }

            while (true) {
                $data = fread($client, 4096);
                if ($data === '' || $data === false) {
                    break;
                }

                $feed = cloudrmi_protocol_feed($ctx, $data);
                if ($feed['status'] !== RMI_OK) {
                    fwrite($client, "400 {$feed['err_message']}\n");
                    break;
                }

                while (true) {
                    $next = cloudrmi_protocol_next_event($ctx);
                    if ($next['status'] === RMI_E_AGAIN) {
                        break;
                    }
                    if ($next['status'] !== RMI_OK || $next['event'] === null) {
                        fwrite($client, "400 {$next['err_message']}\n");
                        break 2;
                    }

                    $event = $next['event'];
                    if ((int)$event['type'] !== RMI_EVENT_CALL) {
                        fwrite($client, "400 unexpected event\n");
                        break 2;
                    }

                    $response = $handler($event);
                    if (!is_array($response) || !isset($response['status'], $response['body'])) {
                        fwrite($client, "500 invalid handler response\n");
                        break 2;
                    }

                    $status = (int)$response['status'];
                    $body = (string)$response['body'];
                    fwrite($client, sprintf("%d %s\n", $status, $body));
                }
            }

            cloudrmi_protocol_destroy($ctx);
            fclose($client);
        }
    }
}
