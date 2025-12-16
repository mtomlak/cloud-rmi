<?php
namespace ProfiWeb\Cloud\Infrastructure\System\IO;

/**
 * Provides a stream for a file, supporting both synchronous and asynchronous 
 * read and write operations.
 * 
 * TODO: We can create and implement a Stream Interface  
 * 
 * Inspiration for further methods:
 *  https://msdn.microsoft.com/en-us/library/system.io.filestream(v=vs.110).aspx
 *  
 * @copyright Copyright (C) 2015 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class FileStream
{
	protected $handle;
	protected $mode;
	protected $isOpen;
	
	/**
	 * Constructs new object of this class
	 * 
	 */
	public function __construct(FileHandle $handle, FileMode $mode)
	{
		$this->setFileHandle($handle);
		$this->setFileMode($mode);
		$this->setOpen();
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
	 * Writes to the stream
	 *
	 * We can add optional parameters like max length or offset
	 * 
	 * @return bool          	
	 */	 	 	
	public function write($string)
	{
		fwrite($this->getFileHandle()->getNativeHandle(), $string);

		return $this;
	}
	
	 	 	 	
	/**
	 * Gets a value that determines whether the current stream can time out.
	 *
	 * @return bool 
	 */	 	 	
	public function canRead($mode)
	{
	}
	
	/**
	 * Gets a value indicating whether the current stream supports writing.
	 *
	 * @return bool          	
	 */	 	 	
	public function canWrite()
	{
	}
	
	/**
	 * Gets a value indicating whether the current stream supports seeking.
	 *
	 * @return bool
	 */	 	 	
	public function canSeek()
	{
	}
	
	/**
	 * Releases all resources this handle represents
	 * 
	 */
	public function close()
	{
		$this->handle->close();
	}	

	/**
	 * Returns handle object of the this stream
	 * 
	 */
	public function getFileHandle()
	{
		return $this->handle;
	}

	/**
	 * Sets a file handle
	 *
	 */	 	 
	protected function setFileHandle(FileHandle $handle)
	{
		$this->handle = $handle;

		return $this;
	}
		
	/**
	 * Sets a POSIX access mode of the file
	 *
	 */	 	 
	protected function setFileMode(FileMode $mode)
	{
		$this->mode = $mode;

		return $this;
	}
	
	/**
	 * Sets an internal status of stream to open
	 *
	 */	 	 
	protected function setOpen()
	{
		$this->isOpen = true;

		return $this;
	}
	
	/**
	 * Sets an internal status of stream to closed
	 *
	 */	 	 
	protected function setClosed(FileMode $mode)
	{
		$this->isOpen = false;

		return $this;
	}
	
	/**
	 * Checks whether this stream is not open
	 *
	 * @return bool	 
	 */	 	 
	protected function isOpen()
	{
		return $this->isOpen;
	} 
	
	/**
	 * Sets an internal status of stream to closed
	 *
	 * @throws IOException	 
	 */	 	 
	protected function assertIsOpen()
	{
		if (!$this->isOpen())
			throw new IOException('Cannot execute operation: stream not open');
	}
	
	
	//	new ideas: lastModified(), length() or size(), list(), 
}
