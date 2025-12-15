<?php

namespace RemoteMethodInvocation;

class SimpleRMIClient
{
    /** @var ConnectionInterface */
    private $connection;

    /**
     * @param ConnectionInterface|string $connection Either an existing connection or hostname
     * @param int $port Ignored when a ConnectionInterface instance is provided
     * @param float $timeout
     */
    public function __construct($connection, int $port = 9501, float $timeout = 3.0)
    {
        if ($connection instanceof ConnectionInterface) {
            $this->connection = $connection;
        } else {
            $this->connection = new SimpleRMIConnection((string)$connection, $port, $timeout);
        }
    }

    /**
     * Perform a remote call using either legacy inline args or PAYLOAD framing.
     *
     * @param string $interface
     * @param string $method
     * @param string $args Raw argument buffer
     * @param array $options ['payload' => bool, 'encoding' => string]
     *
     * @return string Decoded response body
     * @throws RemoteException
     */
    public function call(string $interface, string $method, string $args = '', array $options = []): string
    {
        $usePayload = !empty($options['payload']);
        $encoding = $options['encoding'] ?? 'RAW';

        if ($usePayload) {
            $this->sendPayloadCall($interface, $method, $encoding, $args);
        } else {
            $this->sendLegacyCall($interface, $method, $args);
        }

        return $this->readResponse();
    }

    private function sendLegacyCall(string $interface, string $method, string $args): void
    {
        $payload = base64_encode($args);
        $line = sprintf("CALL %s %s %s\n", $interface, $method, $payload);
        $this->connection->write($line);
    }

    private function sendPayloadCall(string $interface, string $method, string $encoding, string $payload): void
    {
        $header = sprintf("CALL %s %s\nPAYLOAD %s %d\n", $interface, $method, $encoding, strlen($payload));
        $this->connection->write($header);
        if ($payload !== '') {
            $this->connection->write($payload);
        }
    }

    /**
     * Read a status response from the server. Legacy behavior expects a status line
     * with optional base64 payload. Binary payload responses must be accompanied by
     * the PAYLOAD header.
     */
    private function readResponse(): string
    {
        $line = $this->connection->readLine();
        if ($line === false) {
            throw new RemoteMethodInvocationException('Unexpected EOF while waiting for response');
        }

        if (!preg_match('/^(\d{3})\s*(.*)$/', trim($line), $matches)) {
            throw new RemoteMethodInvocationException('Malformed response from server');
        }

        $status = (int)$matches[1];
        $body = $matches[2] ?? '';

        if ($status !== 200) {
            throw new RemoteErrorException($status, $body);
        }

        // If server chooses payload framing for responses, expect PAYLOAD header next
        if (strncasecmp($body, 'PAYLOAD ', 8) === 0) {
            $parts = explode(' ', $body);
            if (count($parts) < 3) {
                throw new RemoteMethodInvocationException('Invalid PAYLOAD header');
            }
            $encoding = $parts[1];
            $length = (int)$parts[2];
            $raw = $this->connection->readBytes($length);
            return $raw;
        }

        if ($body === '') {
            return '';
        }

        $decoded = base64_decode($body, true);
        if ($decoded === false) {
            throw new RemoteMethodInvocationException('Invalid base64 response payload');
        }

        return $decoded;
    }

    public function close(): void
    {
        $this->connection->close();
    }
}
