<?php
namespace ProfiWeb\CloudRMI;

/**
 * Represents a connection to a CloudRMI server.
 *  
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license BSD License
 */
class CloudRMIConnection implements ConnectionInterface
{
	protected $host;
	protected $port;
	protected $socket;
	protected $unixSocketPath;
	
	public function __construct($host, $port, $socket, $unixSocketPath = null)
	{
		$this->host = $host;
		$this->port = $port;
		$this->socket = $socket;
		$this->unixSocketPath = $unixSocketPath;
	}
	
	public function getSocket()
	{
		return $this->socket;
	}

	public function getHost()
	{
		return $this->host;
	}

	public function getPort()
	{
		return $this->port;
	}

	public function isUnixSocket()
	{
		return (!is_null($this->unixSocketPath) && $this->unixSocketPath !== '');
	}

	public function getUnixSocketPath()
	{
		return $this->unixSocketPath;
	}

	public function getEndpointDescription()
	{
		if ($this->isUnixSocket()) {
			if (stripos($this->unixSocketPath, 'unix://') === 0)
				return $this->unixSocketPath;

			return 'unix://' . $this->unixSocketPath;
		}

		return $this->host . ':' . $this->port;
	}
}
