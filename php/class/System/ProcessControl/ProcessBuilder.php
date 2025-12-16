<?php
namespace ProfiWeb\Cloud\Infrastructure\System\ProcessControl;

use ProfiWeb\Cloud\Infrastructure\System\IO\StreamHandle;
use ProfiWeb\Cloud\Infrastructure\System\IO\InputStream;
use ProfiWeb\Cloud\Infrastructure\System\IO\OutputStream;
use ProfiWeb\Cloud\Infrastructure\System\IO\FileStream;

/**
 * Provides a container for a OS-specific stream handle.
 *  
 * @copyright Copyright (C) 2015 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class ProcessBuilder
{   
    protected $command;
    protected $cwd;
    protected $env;
    protected $pipeSpec;

    /**
     * Constructs new ProcessBuilder from command and its args as string
     * 
     */
    public function __construct($command = null)
    {
        $this->setCommand($command);
        $this->setCurrentEnv();
        $this->setCurrentCwd();
        $this->setDefaultPipeSpec();
    }

    /**
     * Starts a new process using the attributes of this process builder.
     * 
     */
    public function start()
    {
        if (!$this->getCommand())
            throw new \Exception("No command to execute");    

        $resource = proc_open(
            $this->getCommand(), 
            $this->getPipeSpec(),
            $pipes,
            $this->getCwd(),
            $this->getEnv());

        if (!is_resource($resource))
            throw new \Exception("Failed to execute process: " . $this->getCommand());

        return new Process(
            new ProcessHandle($resource),
            new OutputStream(new StreamHandle((isset($pipes[0])) ? $pipes[0] : null)),  // child's std in
            new InputStream(new StreamHandle((isset($pipes[1])) ? $pipes[1] : null)),   // child's std out
            new InputStream(new StreamHandle((isset($pipes[2])) ? $pipes[2] : null))    // child's std err
            );
    }
        
    /**
     * 
     * todo: setDirectory, getEnvironment, redirectOutput ...
     *  ...
     */

    /**
     * Gets the value of command.
     *
     * @return mixed
     */
    public function getCommand()
    {
        return $this->command;
    }

    /**
     * Gets the value of cwd.
     *
     * @return mixed
     */
    public function getCwd()
    {
        return $this->cwd;
    }

    
    /**
     * Gets the value of env.
     *
     * @return mixed
     */
    public function getEnv()
    {
        return $this->env;
    }

    /**
     * Sets the value of command.
     *
     * @param mixed $command the command
     *
     * @return self
     */
    public function setCommand($command)
    {
        $this->command = $command;

        return $this;
    }

    /**
     * Sets the value of cwd.
     *
     * @param mixed $cwd the cwd
     *
     * @return self
     */
    public function setCwd($cwd)
    {
        $this->cwd = $cwd;

        return $this;
    }

    /**
     * Sets working directory of a new child process same as the one of parent.
     *
     * @param mixed $cwd the cwd
     *
     * @return self
     */
    public function setCurrentCwd()
    {
        // null tells PHP to use parent's value
        $this->cwd = null;          //  realpath(getcwd());

        return $this;
    }

    /**
     * Sets working directory to '/'
     *
     * @param mixed $cwd the cwd
     *
     * @return self
     */
    public function setCleanCwd()
    {
        // usually '/' is used in *nix as cwd to predictably launch services
        $this->cwd = '/';

        return $this;
    }

    /**
     * Sets the of environment of a new process
     *
     * @param mixed $env the env
     *
     * @return self
     */
    public function setEnv($env)
    {
        $this->env = $env;

        return $this;
    }

    /**
     * Sets the environment of a new process same as the environment of current
     * process.
     *
     * @param mixed $env the env
     *
     * @return self
     */
    public function setCurrentEnv()
    {
        $this->env = null;

        return $this;
    }

    /**
     * Sets environment of a new script to be clean, as predictable as possible.
     * Useful for launching system services and init scripts.
     *
     * @param mixed $env the env
     *
     * @return self
     */
    public function setCleanEnv()
    {
        $allovedVars = [ 'HOME', 'LOGNAME', 'PWD', 'SHELL', 'TERM', 'USER' ];
        $this->env = array();

        foreach($allovedVars as $name)
            if ($value = getenv($name))
                $this->env[$name] = $value;

        return $this;
    }

    // experimental
    
    /**
     * Gets the value of inputStream.
     *
     * @return mixed
     */
    public function redirectInput($stream)
    {
        return $this->pipeSpec[0] = $this->getStreamResource($stream);
    }

    /**
     * Gets the value of outputStream.
     *
     * @return mixed
     */
    public function redirectOutput($stream)
    {
        return $this->pipeSpec[1] = $this->getStreamResource($stream);
    }

    /**
     * Changes the value of errorStream.
     *
     * @return mixed
     */
    public function redirectError($stream)
    {
        return $this->pipeSpec[2] = $this->getStreamResource($stream);
    }


    protected function setDefaultPipeSpec()
    {
        $this->pipeSpec = array(
           0 => array("pipe", "r"), 
           1 => array("pipe", "w"),
           2 => array("pipe", "w")
           );
    }

    /**
     * Returns child process pipe descriptors in the format of $descriptorspec 
     * attribute for proc_open PHP function.
     * 
     */
    protected function getPipeSpec()
    {
        return $this->pipeSpec;
    }

    /**
     * Tries to get PHP native stream resource from mixed kind of streams
     * 
     */
    protected function getStreamResource($stream)
    {
        if ($stream instanceof FileStream)
            return $stream->getFileHandle()->getNativeHandle();

        if ($stream instanceof InputStream || $stream instanceof OutputStream)
            return $stream->getHandle()->getNativeHandle();

        if (is_resource($stream))
            return $stream;

        throw new \Exception("Unrecognized type of stream resource given");
    }

}
