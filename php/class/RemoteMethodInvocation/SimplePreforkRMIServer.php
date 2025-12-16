<?php
namespace ProfiWeb\Cloud\Infrastructure\Messaging\RemoteMethodInvocation;

use ProfiWeb\Cloud\Infrastructure\System\ProcessControl\ProcessFork;
use ProfiWeb\Cloud\Infrastructure\System\IO\StreamHandle;
use ProfiWeb\Cloud\Infrastructure\System\IO\InputStream;
use ProfiWeb\Cloud\Infrastructure\System\IO\OutputStream;
use ProfiWeb\Cloud\Infrastructure\System\IO\FileInfo;
use ProfiWeb\Cloud\Infrastructure\System\IO\IOException;

/**
 * UNDER CONSTRUCTION * 
 * Simple implementation of Remote Method Invocation server.
 *
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak
 * @license Proprietary
 */
class SimplePreforkRMIServer extends SimplePreforkRMIServer
{
	// Configuration parameters
	protected $minSpareWorkers = 2;
	protected $fifoDirPath;
	// Other vars
	protected $fifoDir;
	protected $workers;

	public function __construct()
	{
		if (!$this->hasForkSupport())
			throw new \Exception("Cannot boot prefork type of RMI server without fork() support");

		parent::__construct();

		$this->setFifoDir();
	}


	public function start()
	{
		// set up the server
		$this->registerShutdownHandler();
		$this->openServerSocket();
		$this->startedAt = time();
		$this->serverPid = posix_getpid();
		$this->log($this->contextName . " service ready");
		$this->proc = new ProcessFork();

		// start up minimum number of worker proccesses
		$this->startWorkers();

		// main program loop
		while (!$this->doShutdown) {
			$this->acceptNewConnections();
			$this->proc->dispatchSignals();
		}

		$this->closeServerSocket();
		$this->doShutdown = false;
	}


	protected function startWorkers()
	{
		for ($i = 0; $i < $this->minSpareWorkers; $i++)
			$this->addWorker();
	}

	protected function addWorker()
	{
		$pid = $this->proc->fork();

		if ($this->proc->isParent()) {
			$this->log("Forked new child proccess with PID $pid");
			$this->closeSocket($socket);
			return;
		}
	}

	protected function setFifoDir()
	{
		if ($this->fifoDirPath)
            $path = $this->fifoDirPath;
        else
            $path = sys_get_temp_dir();  // fallback

        return new FileInfo($path);
	}




}
