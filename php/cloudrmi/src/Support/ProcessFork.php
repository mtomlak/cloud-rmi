<?php
namespace ProfiWeb\CloudRMI\Support;

/**
 * Helps to fork child processes and simple management of them
 *  
 * @copyright Copyright (C) 2016 ProfiWeb sro (http://profiweb.biz/).
 * @author Marian Tomlak <marian@profiweb.biz>
 * @license BSD License
 */
class ProcessFork 
{
    protected $children = [];
    protected $isChild = false;
    protected $handlerHasStarted = false;
    protected $cleanupInterval = 3;

    /**
     * Forks a new child process.
     * 
     */
    public function fork()
    {
        $this->startSignalHandler();            // register child signal handler

        if (($childPid = pcntl_fork()) == -1)
            throw new \Exception("Failed to fork");

        else if ($childPid)
            $this->children[] = $childPid;

        else
            $this->isChild = true;

        return $childPid;
    }       

    public function isParent()
    {
        return !$this->isChild();   
    }

    public function isChild()
    {
        return $this->isChild;   
    }

    /**
     * Called on parent waits for child to exit, called on child exits process.
     * 
     */
    public function join()
    {
        if ($this->isChild())
            exit;

        else
            foreach($this->children as $childPid)
                pcntl_waitpid($childPid, $status);

        return $status;
    }

    /**
     * Checks for and removes zombie children
     * 
     */
    public function collectChildren()
    {
        pcntl_wait($status, WNOHANG|WUNTRACED);
    }

    /**
     * Calls signal handlers for pending signals. Deprecated, calling this function
     * regularly (from main loop for example) can avoid use of declare(ticks =1)
     * when running on PHP older than 7.1.0.
     * 
     */
    public function dispatchSignals()
    {
        if (!function_exists('pcntl_async_signals'))
            pcntl_signal_dispatch();
    }

    /**
     * Handles signals from children.
     * 
     */
    public function onSignalRecieved($signo) 
    {
        switch ($signo) {
            case SIGCHLD:
            case SIGALRM:
                $this->collectChildren();
                break;
        }
    }

    /**
     * Collects children signals when script shutting down
     * 
     */
    public function onShutdown()
    {
        if ($this->isParent()) {
            $this->join();
            $this->collectChildren();
        }
    }

    /**
     * Registers a signal handler and shutdown function. For signal handler
     * to work it requires method dispatchSignals() to be called in loop,
     * or declare(ticks = 1) needs to be defined.
     * 
     */
    protected function startSignalHandler() 
    {
        if (!$this->handlerHasStarted) {
            // signals from exited children
            pcntl_signal(SIGCHLD, array($this, 'onSignalRecieved'));
            // timer for cleanups
            pcntl_signal(SIGALRM, array($this, 'onSignalRecieved'));
            pcntl_alarm($this->cleanupInterval);
            // default shutdown hook
            register_shutdown_function(array($this, 'onShutdown'));

            // enable asynchronous signal handling if available
            if (function_exists('pcntl_async_signals'))
                pcntl_async_signals(true);

            $this->handlerHasStarted = true;
        }
    }
}
