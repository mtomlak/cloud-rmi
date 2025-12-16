<?php
namespace ProfiWeb\Cloud\Infrastructure\System\IO;

/**
 * Provides a container for a mode in which a file can be open.
 * 
 * TODO: We can create and extend a Stream class  
 * 
 * Inspiration for further methods:
 *  https://msdn.microsoft.com/en-us/library/system.io.filestream(v=vs.110).aspx
 *  
 * @copyright Copyright (C) 2015 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class FileMode
{
	const MODE_OPEN_READ = 'r';
	const MODE_OPEN_WRITE = 'w';
	const MODE_OPEN_APPEND = 'a';
	const MODE_OPEN_MIXED = 'w+';
	
	protected $mode;
		

	/**
	 * Constructs new object of this class
	 * 
	 */
	public function __construct($nativeModeCode)
	{
		$this->setNative($nativeModeCode);	
	}
	 	 	 	
	/**
	 * Gets a value of operation code that represents current file mode.
	 *
	 * @return mixed 
	 */	 	 	
	public function getNative()
	{
		return $this->mode;
	}
	
	/**
	 * Sets a value of operation code that represents current file mode. This
	 * method is protected as this object should be immutable.	 
	 *
	 * @return mixed 
	 */	 	 	
	protected function setNative($mode)
	{
		$this->validateNativeMode($mode);
		$this->mode = $mode;
	}
	
	protected function validateNativeMode($nativeModeCode)
	{
		switch($nativeModeCode) {
			case Self::MODE_OPEN_READ:
			case Self::MODE_OPEN_WRITE:
			case Self::MODE_OPEN_APPEND:
			case Self::MODE_OPEN_MIXED:
				return;
				
			default:
				throw new IOException('Invalid file open mode code given: '
					. $nativeModeCode);
		}
	}

}
