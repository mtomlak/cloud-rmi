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
	protected $protocolVersion = null;
	protected $advertisedMaxProtocol = null;
	protected $capabilities = [];
	protected $supportsAsyncCapability = null;
	protected $connectedAt = null;
	
	public function __construct($host, $port, $socket, $unixSocketPath = null)
	{
		$this->host = $host;
		$this->port = $port;
		$this->socket = $socket;
		$this->unixSocketPath = $unixSocketPath;
		$this->connectedAt = time();
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

	public function getProtocolVersion()
	{
		return $this->protocolVersion;
	}

	public function setProtocolVersion($protocolVersion)
	{
		$this->protocolVersion = is_null($protocolVersion)
			? null
			: (int) $protocolVersion;
	}

	public function getAdvertisedMaxProtocol()
	{
		return $this->advertisedMaxProtocol;
	}

	public function setAdvertisedMaxProtocol($advertisedMaxProtocol)
	{
		$this->advertisedMaxProtocol = is_null($advertisedMaxProtocol)
			? null
			: (int) $advertisedMaxProtocol;
	}

	public function getCapabilities()
	{
		return $this->capabilities;
	}

	public function setCapabilities(array $capabilities)
	{
		$this->capabilities = $capabilities;
	}

	public function supportsAsyncCapability()
	{
		return $this->supportsAsyncCapability;
	}

	public function setSupportsAsyncCapability($supportsAsyncCapability)
	{
		$this->supportsAsyncCapability = is_null($supportsAsyncCapability)
			? null
			: (bool) $supportsAsyncCapability;
	}

	public function getConnectedAt()
	{
		return $this->connectedAt;
	}

	public function setConnectedAt($connectedAt)
	{
		$this->connectedAt = is_null($connectedAt)
			? null
			: (int) $connectedAt;
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
