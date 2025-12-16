<?php
namespace ProfiWeb\Cloud\Infrastructure\System\ProcessControl;

use ProfiWeb\Cloud\Infrastructure\System\IO\StreamHandle;
use ProfiWeb\Cloud\Infrastructure\System\IO\InputStream;
use ProfiWeb\Cloud\Infrastructure\System\IO\OutputStream;

/**
 * Represents a processs in a system, usually created by ProcessBuilder.
 *  
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class Process
{	
	protected $inputStream;
	protected $outputStream;
	protected $errorStream;
	protected $handle;


	/**
	 * Constructs new object of this class. It should be created only
	 * by ProcessBuilder.
	 * 
	 *  @param ProcessHandle
	 *  @param OutputStream  output stream connected to the normal input of the subprocess
	 *  @param InputStream   input stream connected to the normal output of the subprocess.
	 *  @param InputStream   input stream connected to the error output of the subprocess.
	 */
	public function __construct(
		ProcessHandle $handle,
		OutputStream $outputStream,
		InputStream $inputStream,
		InputStream $errorStream
		)
	{
		// todo: use setters
		$this->handle = $handle;
		$this->inputStream = $inputStream;
		$this->outputStream = $outputStream;
		$this->errorStream = $errorStream;
	}
	
	/**
	 * Destroys the process if it's stil running
	 * 
	 */
	public function destroy()
	{
		return $this->getHandle()->close();
	}	 	

	/**
	 * Releases all resources this handle represents
	 * 
	 */
	public function exitValue()
	{
		//return fclose($this->handle);
	}	 	

    /**
     * Gets the value of errorStream.
     *
     * @return mixed
     */
    public function getErrorStream()
    {
        return $this->errorStream;
    }

    /**
     * Gets the value of inputStream.
     *
     * @return mixed
     */
    public function getInputStream()
    {
        return $this->inputStream;
    }

    /**
     * Gets the value of outputStream.
     *
     * @return mixed
     */
    public function getOutputStream()
    {
        return $this->outputStream;
    }

	/**
	 * Releases all resources this handle represents
	 * 
	 */
	public function waitFor()
	{
		return $this->getHandle()->waitFor();
	}	

	/**
	 * Sets a file handle
	 *
	 */	 	 
	protected function getHandle()
	{
		return $this->handle;
	} 
}
