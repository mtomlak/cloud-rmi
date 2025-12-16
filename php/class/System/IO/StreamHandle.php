<?php
namespace ProfiWeb\Cloud\Infrastructure\System\IO;

/**
 * Provides a container for a OS-specific stream handle.
 *  
 * @copyright Copyright (C) 2015 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class StreamHandle
{	
	protected $handle;

	/**
	 * Constructs new object of this class
	 * 
	 */
	public function __construct($nativeHandle)
	{
		$this->handle = $nativeHandle;	
	}
	 	 	 	
	/**
	 * Gets PHP native file-handle
	 *
	 * @return mixed 
	 */	 	 	
	public function getNativeHandle()
	{
		return $this->handle;
	}
	
	/**
	 * Releases all resources this handle represents
	 * 
	 */
	public function close()
	{
		fclose($this->handle);
		$this->handle = null;
	}	 	 	
}
