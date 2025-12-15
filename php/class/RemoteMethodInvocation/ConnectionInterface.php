<?php

namespace RemoteMethodInvocation;

interface ConnectionInterface
{
    /**
     * Write raw bytes to the connection.
     */
    public function write(string $data): void;

    /**
     * Read exactly $length bytes from the connection.
     */
    public function readBytes(int $length): string;

    /**
     * Read a single line terminated by a newline character.
     * Returns false on EOF.
     */
    public function readLine();

    /**
     * Close the connection.
     */
    public function close(): void;
}
