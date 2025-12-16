<?php
namespace ProfiWeb\Cloud\Infrastructure\System\IO;

/**
 * Represents the most simple kind of input stream. 
 *  
 * @copyright Copyright (C) 2015 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class InputStream
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
	 * Reads from the stream and returns the content as a string. 
	 *
	 * @param int $bytes 	The maximum bytes to read
	 * @param int $offset 	Start reading from the given offset	 	 
	 * @return string 
	 */	 	 	
	public function read($bytes = -1, $offset = -1)
	{
		return stream_get_contents(
			$this->handle->getNativeHandle(),
			$bytes,
			$offset
			);	
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
