<?php
namespace ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation;

/**
 * Represents connection to Simle RMI server
 *  
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license BSD License
 */
class SimpleRMIConnection implements ConnectionInterface
{
	protected $host;
	protected $port;
	protected $socket;
	
	public function __construct($host, $port, $socket)
	{
		$this->host = $host;
		$this->port = $port;
		$this->socket = $socket;
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
}
