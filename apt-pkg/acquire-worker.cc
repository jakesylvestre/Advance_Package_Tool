// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: acquire-worker.cc,v 1.16 1998/12/14 06:54:41 jgg Exp $
/* ######################################################################

   Acquire Worker 

   The worker process can startup either as a Configuration prober
   or as a queue runner. As a configuration prober it only reads the
   configuration message and 
   
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#ifdef __GNUG__
#pragma implementation "apt-pkg/acquire-worker.h"
#endif
#include <apt-pkg/acquire-worker.h>
#include <apt-pkg/acquire-item.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/error.h>
#include <apt-pkg/fileutl.h>
#include <strutl.h>

#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <wait.h>
#include <stdio.h>
									/*}}}*/

// Worker::Worker - Constructor for Queue startup			/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgAcquire::Worker::Worker(Queue *Q,MethodConfig *Cnf,
			   pkgAcquireStatus *Log) : Log(Log)
{
   OwnerQ = Q;
   Config = Cnf;
   Access = Cnf->Access;
   CurrentItem = 0;

   Construct();   
}
									/*}}}*/
// Worker::Worker - Constructor for method config startup		/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgAcquire::Worker::Worker(MethodConfig *Cnf)
{
   OwnerQ = 0;
   Config = Cnf;
   Access = Cnf->Access;
   CurrentItem = 0;
   
   Construct();   
}
									/*}}}*/
// Worker::Construct - Constructor helper				/*{{{*/
// ---------------------------------------------------------------------
/* */
void pkgAcquire::Worker::Construct()
{
   NextQueue = 0;
   NextAcquire = 0;
   Process = -1;
   InFd = -1;
   OutFd = -1;
   OutReady = false;
   InReady = false;
   Debug = _config->FindB("Debug::pkgAcquire::Worker",false);
}
									/*}}}*/
// Worker::~Worker - Destructor						/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgAcquire::Worker::~Worker()
{
   close(InFd);
   close(OutFd);
   
   if (Process > 0)
   {
      kill(Process,SIGINT);
      if (waitpid(Process,0,0) != Process)
	 _error->Warning("I waited but nothing was there!");
   }   
}
									/*}}}*/
// Worker::Start - Start the worker process				/*{{{*/
// ---------------------------------------------------------------------
/* This forks the method and inits the communication channel */
bool pkgAcquire::Worker::Start()
{
   // Get the method path
   string Method = _config->FindDir("Dir::Bin::Methods") + Access;
   if (FileExists(Method) == false)
      return _error->Error("The method driver %s could not be found.",Method.c_str());

   if (Debug == true)
      clog << "Starting method '" << Method << '\'' << endl;

   // Create the pipes
   int Pipes[4] = {-1,-1,-1,-1};
   if (pipe(Pipes) != 0 || pipe(Pipes+2) != 0)
   {
      _error->Errno("pipe","Failed to create IPC pipe to subprocess");
      for (int I = 0; I != 4; I++)
	 close(Pipes[I]);
      return false;
   }
   for (int I = 0; I != 4; I++)
      SetCloseExec(Pipes[0],true);
   
   // Fork off the process
   Process = fork();
   if (Process < 0)
   {
      cerr << "FATAL -> Failed to fork." << endl;
      exit(100);
   }

   // Spawn the subprocess
   if (Process == 0)
   {
      // Setup the FDs
      dup2(Pipes[1],STDOUT_FILENO);
      dup2(Pipes[2],STDIN_FILENO);
      dup2(((filebuf *)clog.rdbuf())->fd(),STDERR_FILENO);
      SetCloseExec(STDOUT_FILENO,false);
      SetCloseExec(STDIN_FILENO,false);      
      SetCloseExec(STDERR_FILENO,false);
      
      const char *Args[2];
      Args[0] = Method.c_str();
      Args[1] = 0;
      execv(Args[0],(char **)Args);
      cerr << "Failed to exec method " << Args[0] << endl;
      _exit(100);
   }

   // Fix up our FDs
   InFd = Pipes[0];
   OutFd = Pipes[3];
   SetNonBlock(Pipes[0],true);
   SetNonBlock(Pipes[3],true);
   close(Pipes[1]);
   close(Pipes[2]);
   OutReady = false;
   InReady = true;
   
   // Read the configuration data
   if (WaitFd(InFd) == false ||
       ReadMessages() == false)
      return _error->Error("Method %s did not start correctly",Method.c_str());

   RunMessages();
   if (OwnerQ != 0)
      SendConfiguration();
   
   return true;
}
									/*}}}*/
// Worker::ReadMessages - Read all pending messages into the list	/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgAcquire::Worker::ReadMessages()
{
   if (::ReadMessages(InFd,MessageQueue) == false)
      return MethodFailure();
   return true;
}
									/*}}}*/
// Worker::RunMessage - Empty the message queue				/*{{{*/
// ---------------------------------------------------------------------
/* This takes the messages from the message queue and runs them through
   the parsers in order. */
bool pkgAcquire::Worker::RunMessages()
{
   while (MessageQueue.empty() == false)
   {
      string Message = MessageQueue.front();
      MessageQueue.erase(MessageQueue.begin());

      if (Debug == true)
	 clog << " <- " << Access << ':' << QuoteString(Message,"\n") << endl;
      
      // Fetch the message number
      char *End;
      int Number = strtol(Message.c_str(),&End,10);
      if (End == Message.c_str())
	 return _error->Error("Invalid message from method %s: %s",Access.c_str(),Message.c_str());

      string URI = LookupTag(Message,"URI");
      pkgAcquire::Queue::QItem *Itm = 0;
      if (URI.empty() == false)
	 Itm = OwnerQ->FindItem(URI,this);
      
      // Determine the message number and dispatch
      switch (Number)
      {
	 // 100 Capabilities
	 case 100:
	 if (Capabilities(Message) == false)
	    return _error->Error("Unable to process Capabilities message from %s",Access.c_str());
	 break;
	 
	 // 101 Log
	 case 101:
	 if (Debug == true)
	    clog << " <- (log) " << LookupTag(Message,"Message") << endl;
	 break;
	 
	 // 102 Status
	 case 102:
	 Status = LookupTag(Message,"Message");
	 break;
	    
	 // 200 URI Start
	 case 200:
	 {
	    if (Itm == 0)
	    {
	       _error->Error("Method gave invalid 200 URI Start message");
	       break;
	    }
	    
	    CurrentItem = Itm;
	    CurrentSize = 0;
	    TotalSize = atoi(LookupTag(Message,"Size","0").c_str());
	    Itm->Owner->Start(Message,atoi(LookupTag(Message,"Size","0").c_str()));
	    
	    if (Log != 0)
	       Log->Fetch(*Itm);

	    break;
	 }
	 
	 // 201 URI Done
	 case 201:
	 {
	    if (Itm == 0)
	    {
	       _error->Error("Method gave invalid 201 URI Done message");
	       break;
	    }

	    pkgAcquire::Item *Owner = Itm->Owner;
	    pkgAcquire::ItemDesc Desc = *Itm;
	    OwnerQ->ItemDone(Itm);
	    Owner->Done(Message,atoi(LookupTag(Message,"Size","0").c_str()),
					  LookupTag(Message,"MD5-Hash"));
	    ItemDone();
	    
	    // Log that we are done
	    if (Log != 0)
	    {
	       if (StringToBool(LookupTag(Message,"IMS-Hit"),false) == true ||
		   StringToBool(LookupTag(Message,"Alt-IMS-Hit"),false) == true)
		  Log->IMSHit(Desc);
	       else
		  Log->Done(Desc);
	    }	    
	    break;
	 }	 
	 
	 // 400 URI Failure
	 case 400:
	 {
	    if (Itm == 0)
	    {
	       _error->Error("Method gave invalid 400 URI Failure message");
	       break;
	    }

	    pkgAcquire::Item *Owner = Itm->Owner;
	    pkgAcquire::ItemDesc Desc = *Itm;
	    OwnerQ->ItemDone(Itm);
	    Owner->Failed(Message);
	    ItemDone();
	    
	    if (Log != 0)
	       Log->Fail(Desc);
	    
	    break;
	 }	 
	 
	 // 401 General Failure
	 case 401:
	 _error->Error("Method %s General failure: %s",LookupTag(Message,"Message").c_str());
	 break;
	 
	 // 403 Media Change
	 case 403:
	 MediaChange(Message); 
	 break;
      }      
   }
   return true;
}
									/*}}}*/
// Worker::Capabilities - 100 Capabilities handler			/*{{{*/
// ---------------------------------------------------------------------
/* This parses the capabilities message and dumps it into the configuration
   structure. */
bool pkgAcquire::Worker::Capabilities(string Message)
{
   if (Config == 0)
      return true;
   
   Config->Version = LookupTag(Message,"Version");
   Config->SingleInstance = StringToBool(LookupTag(Message,"Single-Instance"),false);
   Config->Pipeline = StringToBool(LookupTag(Message,"Pipeline"),false);
   Config->SendConfig = StringToBool(LookupTag(Message,"Send-Config"),false);
   Config->LocalOnly = StringToBool(LookupTag(Message,"Local-Only"),false);

   // Some debug text
   if (Debug == true)
   {
      clog << "Configured access method " << Config->Access << endl;
      clog << "Version:" << Config->Version << " SingleInstance:" <<
	 Config->SingleInstance << 
	 " Pipeline:" << Config->Pipeline << " SendConfig:" << 
	 Config->SendConfig << endl;
   }
   
   return true;
}
									/*}}}*/
// Worker::MediaChange - Request a media change				/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgAcquire::Worker::MediaChange(string Message)
{
   if (Log == 0 || Log->MediaChange(LookupTag(Message,"Media"),
				    LookupTag(Message,"Drive")) == false)
   {
      char S[300];
      sprintf(S,"603 Media Changed\nFailed: true\n\n");
      if (Debug == true)
	 clog << " -> " << Access << ':' << QuoteString(S,"\n") << endl;
      OutQueue += S;
      OutReady = true;
      return true;
   }

   char S[300];
   sprintf(S,"603 Media Changed\n\n");
   if (Debug == true)
      clog << " -> " << Access << ':' << QuoteString(S,"\n") << endl;
   OutQueue += S;
   OutReady = true;
   return true;
}
									/*}}}*/
// Worker::SendConfiguration - Send the config to the method		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgAcquire::Worker::SendConfiguration()
{
   if (Config->SendConfig == false)
      return true;

   if (OutFd == -1)
      return false;
   
   string Message = "601 Configuration\n";
   Message.reserve(2000);

   /* Write out all of the configuration directives by walking the 
      configuration tree */
   const Configuration::Item *Top = _config->Tree(0);
   for (; Top != 0;)
   {
      if (Top->Value.empty() == false)
      {
	 string Line = "Config-Item: " + Top->FullTag() + "=";
	 Line += QuoteString(Top->Value,"\n") + '\n';
	 Message += Line;
      }
      
      if (Top->Child != 0)
      {
	 Top = Top->Child;
	 continue;
      }
      
      while (Top != 0 && Top->Next == 0)
	 Top = Top->Parent;
      if (Top != 0)
	 Top = Top->Next;
   }   
   Message += '\n';

   if (Debug == true)
      clog << " -> " << Access << ':' << QuoteString(Message,"\n") << endl;
   OutQueue += Message;
   OutReady = true; 
   
   return true;
}
									/*}}}*/
// Worker::QueueItem - Add an item to the outbound queue		/*{{{*/
// ---------------------------------------------------------------------
/* Send a URI Acquire message to the method */
bool pkgAcquire::Worker::QueueItem(pkgAcquire::Queue::QItem *Item)
{
   if (OutFd == -1)
      return false;
   
   string Message = "600 URI Acquire\n";
   Message.reserve(300);
   Message += "URI: " + Item->URI;
   Message += "\nFilename: " + Item->Owner->DestFile;
   Message += Item->Owner->Custom600Headers();
   Message += "\n\n";
   
   if (Debug == true)
      clog << " -> " << Access << ':' << QuoteString(Message,"\n") << endl;
   OutQueue += Message;
   OutReady = true;
   
   return true;
}
									/*}}}*/
// Worker::OutFdRead - Out bound FD is ready				/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgAcquire::Worker::OutFdReady()
{
   int Res = write(OutFd,OutQueue.begin(),OutQueue.length());
   if (Res <= 0)
      return MethodFailure();

   // Hmm.. this should never happen.
   if (Res < 0)
      return true;
   
   OutQueue.erase(0,Res);
   if (OutQueue.empty() == true)
      OutReady = false;
   
   return true;
}
									/*}}}*/
// Worker::InFdRead - In bound FD is ready				/*{{{*/
// ---------------------------------------------------------------------
/* */
bool pkgAcquire::Worker::InFdReady()
{
   if (ReadMessages() == false)
      return false;
   RunMessages();
   return true;
}
									/*}}}*/
// Worker::MethodFailure - Called when the method fails			/*{{{*/
// ---------------------------------------------------------------------
/* This is called when the method is belived to have failed, probably because
   read returned -1. */
bool pkgAcquire::Worker::MethodFailure()
{
   _error->Error("Method %s has died unexpectedly!",Access.c_str());
   
   if (waitpid(Process,0,0) != Process)
      _error->Warning("I waited but nothing was there!");
   Process = -1;
   close(InFd);
   close(OutFd);
   InFd = -1;
   OutFd = -1;
   OutReady = false;
   InReady = false;
   OutQueue = string();
   MessageQueue.erase(MessageQueue.begin(),MessageQueue.end());
   
   return false;
}
									/*}}}*/
// Worker::Pulse - Called periodically 					/*{{{*/
// ---------------------------------------------------------------------
/* */
void pkgAcquire::Worker::Pulse()
{
   if (CurrentItem == 0)
      return;
 
   struct stat Buf;
   if (stat(CurrentItem->Owner->DestFile.c_str(),&Buf) != 0)
      return;
   CurrentSize = Buf.st_size;
}
									/*}}}*/
// Worker::ItemDone - Called when the current item is finished		/*{{{*/
// ---------------------------------------------------------------------
/* */
void pkgAcquire::Worker::ItemDone()
{
   CurrentItem = 0;
   CurrentSize = 0;
   TotalSize = 0;
   Status = string();
}
									/*}}}*/
