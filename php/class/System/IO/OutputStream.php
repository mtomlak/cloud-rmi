<?php
namespace ProfiWeb\Cloud\Infrastructure\System\IO;

/**
 * Represents the most simple kind of output stream. 
 *  
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class OutputStream
{
	protected $handle;
	
	/**
	 * Constructs new object of this class
	 * 
	 */
	public function __construct(StreamHandle $handle)
	{
		$this->setHandle($handle);
	}
	
	/**
	 * Writes to the stream
	 *
	 * We can add optional parameters like max length or offset
	 * 
	 * @return bool          	
	 */	 	 	
	public function write($string)
	{
		fwrite($this->getHandle()->getNativeHandle(), $string);
	}
	
	/**
	 * Releases all resources this handle represents
	 * 
	 */
	public function close()
	{
		$this->getHandle()->close();
	}	

	/**
	 * Gets a stream handle
	 *
	 */	 	 
	public function getHandle()
	{
		return $this->handle;
	} 

	/**
	 * Sets a stream handle
	 *
	 */	 	 
	protected function setHandle(StreamHandle $handle)
	{
		$this->handle = $handle;
	} 
}
