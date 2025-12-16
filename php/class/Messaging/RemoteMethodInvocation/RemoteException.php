<?php
namespace ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation;

/**
 * Exception that has been forwarded from remote system. Copies information
 * from original Exception and ensures it's serializable. 
 * 
 * This object has to be serializable so we cannot maintain full stack trace, 
 * as some parts of it, like PDO instances, are not serializable.
 *  
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class RemoteException extends \Exception
{
	protected $contextName;
	protected $host;
	protected $port;
	protected $originalClassName;
	protected $originalTrace;
	protected $originalAttributes;

	/**
	 * Constructs a new exception
	 * 
	 */
	public function __construct(
		\Throwable $originalException,
		$contextName,
		$host,
		$port
		)
	{
		Parent::__construct($originalException->getMessage());
		// overwrite properties with those from original exception,
		// except for stack trace
		$this->code = $originalException->getCode();
		$this->file = $originalException->getFile();
		$this->line = $originalException->getLine();
		// add properties relevant to remote exception
		$this->originalClassName = get_class($originalException);
		$this->originalTrace = $originalException->getTraceAsString();
		$this->contextName = $contextName;
		$this->host = $host;
		$this->port = $port;

		if (method_exists($originalException, 'getAttributes')) {
			$this->originalAttributes = $originalException->getAttributes();
		}
	}
	
	public function getContextName()
	{
		return $this->contextName;
	}
	
	public function getHost()
	{
		return $this->host;
	}
	
	public function getPort()
	{
		return $this->port;
	}

	public function getOriginalClassName()
	{
		return $this->originalClassName;
	}

	public function getOriginalTraceAsString()
	{
		return $this->originalTrace;
	}

	public function getOriginalAttributes()
	{
		return $this->originalAttributes;
	}

	public function __get($name)
	{
		// check if attribute is available in originalAttributes
		if (is_array($this->originalAttributes) && array_key_exists($name, $this->originalAttributes)) {
			return $this->originalAttributes[$name];
		}

		// check if there's a defined property
		if (property_exists($this, $name)) {
			return $this->$name;
		}

		// check getter method
		$method = 'get' . ucfirst($name);
		if (method_exists($this, $method)) {
			return $this->$method();
		}

		return null;
	}
}
