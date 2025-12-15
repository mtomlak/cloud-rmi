<?php

namespace RemoteMethodInvocation;

class SimpleRMIConnection implements ConnectionInterface
{
    /** @var resource */
    private $stream;

    public function __construct(string $host, int $port, float $timeout = 3.0)
    {
        $this->stream = @stream_socket_client("tcp://{$host}:{$port}", $errno, $errstr, $timeout);
        if ($this->stream === false) {
            throw new RemoteMethodInvocationException("Failed to connect: {$errstr}");
        }

        stream_set_blocking($this->stream, true);
        stream_set_timeout($this->stream, $timeout);
    }

    public function write(string $data): void
    {
        $written = 0;
        $length = strlen($data);
        while ($written < $length) {
            $chunk = fwrite($this->stream, substr($data, $written));
            if ($chunk === false) {
                throw new RemoteMethodInvocationException('Failed to write to socket');
            }
            $written += $chunk;
        }
    }

    public function readBytes(int $length): string
    {
        $buffer = '';
        while (strlen($buffer) < $length) {
            $chunk = fread($this->stream, $length - strlen($buffer));
            if ($chunk === '' || $chunk === false) {
                $meta = stream_get_meta_data($this->stream);
                if (!empty($meta['timed_out'])) {
                    throw new RemoteMethodInvocationException('Timed out waiting for payload');
                }
                throw new RemoteMethodInvocationException('Connection closed while reading payload');
            }
            $buffer .= $chunk;
        }

        return $buffer;
    }

    public function readLine()
    {
        return fgets($this->stream);
    }

    public function close(): void
    {
        if (is_resource($this->stream)) {
            fclose($this->stream);
        }
    }
}
