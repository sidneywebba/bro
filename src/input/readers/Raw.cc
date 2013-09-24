// See the file "COPYING" in the main distribution directory for copyright.

#include "Raw.h"
#include "NetVar.h"

#include "../../threading/SerialTypes.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

extern "C" {
#include "setsignal.h"
}

using namespace input::reader;
using threading::Value;
using threading::Field;

const int Raw::block_size = 4096; // how big do we expect our chunks of data to be.
pthread_mutex_t Raw::fork_mutex;

bool Raw::ClassInit()
	{
	return pthread_mutex_init(&fork_mutex, 0) == 0;
	}

Raw::Raw(ReaderFrontend *frontend) : ReaderBackend(frontend)
	{
	file = 0;
	stderrfile = 0;
	forcekill = false;
	separator.assign( (const char*) BifConst::InputRaw::record_separator->Bytes(),
			  BifConst::InputRaw::record_separator->Len());

	sep_length = BifConst::InputRaw::record_separator->Len();

	buf = 0;
	outbuf = 0;
	bufpos = 0;

	stdin_fileno = fileno(stdin);
	stdout_fileno = fileno(stdout);
	stderr_fileno = fileno(stderr);

	childpid = -1;

	stdin_towrite = 0; // by default do not open stdin
	use_stderr = false;
	}

Raw::~Raw()
	{
	DoClose();
	}

void Raw::DoClose()
	{
	if ( file != 0 )
		CloseInput();

	if ( buf != 0 )
		{
		// we still have output that has not been flushed. Throw away.
		delete buf;
		buf = 0;
		}

	if ( execute && childpid > 0 && kill(childpid, 0) == 0 )
		{
		// Kill child process group.
		kill(-childpid, SIGTERM);

		if ( forcekill )
			{
			usleep(200); // 200 msecs should be enough for anyone ;)

			if ( kill(childpid, 0) == 0 ) // perhaps it is already gone
				kill(-childpid, SIGKILL);
			}
		}
	}

void Raw::ClosePipeEnd(int i)
	{
	if ( pipes[i] == -1 )
		return;

	safe_close(pipes[i]);
	pipes[i] = -1;
	}

bool Raw::SetFDFlags(int fd, int cmd, int flags)
	{
	if ( fcntl(fd, cmd, flags) != -1 )
		return true;

	char buf[256];
	strerror_r(errno, buf, sizeof(buf));
	Error(Fmt("failed to set fd flags: %s", buf));
	return false;
	}


bool Raw::LockForkMutex()
	{
	int res = pthread_mutex_lock(&fork_mutex);
	if ( res == 0 )
		return true;

	Error(Fmt("cannot lock fork mutex: %d", res));
	return false;
	}

bool Raw::UnlockForkMutex()
	{
	int res = pthread_mutex_unlock(&fork_mutex);
	if ( res == 0 )
		return true;

	Error(Fmt("cannot unlock fork mutex: %d", res));
	return false;
	}

bool Raw::Execute()
	{
	// AFAICT, pipe/fork/exec should be thread-safe, but actually having
	// multiple threads set up pipes and fork concurrently sometimes
	// results in problems w/ a stdin pipe not ever getting an EOF even
	// though both ends of it are closed.  But if the same threads
	// allocate pipes and fork individually or sequentially, that issue
	// never crops up... ("never" meaning I haven't seen in it in
	// hundreds of tests using 50+ threads where before I'd see the issue
	// w/ just 2 threads ~33% of the time).
	if ( ! LockForkMutex() )
		return false;

	if ( pipe(pipes) != 0 || pipe(pipes+2) || pipe(pipes+4) )
		{
		UnlockForkMutex();
		Error(Fmt("Could not open pipe: %d", errno));
		return false;
		}

	childpid = fork();
	if ( childpid < 0 )
		{
		UnlockForkMutex();
		Error(Fmt("Could not create child process: %d", errno));
		return false;
		}

	else if ( childpid == 0 )
		{
		// we are the child.

		// Obtain a process group w/ child's PID.
		if ( setpgid(0, 0) == -1 )
			_exit(251);

		close(pipes[stdout_in]);
		if ( dup2(pipes[stdout_out], stdout_fileno) == -1 )
			_exit(252);

		close(pipes[stdout_out]);

		close(pipes[stdin_out]);
		if ( stdin_towrite && dup2(pipes[stdin_in], stdin_fileno) == -1 )
			_exit(253);

		close(pipes[stdin_in]);

		close(pipes[stderr_in]);
		if ( use_stderr && dup2(pipes[stderr_out], stderr_fileno) == -1 )
			_exit(254);

		close(pipes[stderr_out]);

		// Signal mask is inherited over fork-exec, so reset any ignored
		// signals to default behavior and unblock any blocked signals.
		setsignal(SIGPIPE, SIG_DFL); // May be ignored when debugging scripts.
		sigset_t mask;
		sigfillset(&mask);
		// Assuming the fork-one model of pthreads' fork(), using sigprocmask()
		// makes sense over pthread_sigmask() here.
		sigprocmask(SIG_UNBLOCK, &mask, 0);

		execl("/bin/sh", "sh", "-c", fname.c_str(), (char*) NULL);
		fprintf(stderr, "Exec failed :(......\n");
		_exit(255);
		}
	else
		{
		// we are the parent

		// Parent also sets child process group immediately to avoid a race.
		if ( setpgid(childpid, childpid) == -1 )
			{
			char buf[256];
			strerror_r(errno, buf, sizeof(buf));
			Warning(Fmt("Could not set child process group: %s", buf));
			}

		if ( ! UnlockForkMutex() )
			return false;

		ClosePipeEnd(stdout_out);

		if ( Info().mode == MODE_STREAM )
			if ( ! SetFDFlags(pipes[stdout_in], F_SETFL, O_NONBLOCK) )
				return false;

		ClosePipeEnd(stdin_in);

		if ( stdin_towrite )
			{
			// Ya, just always set this to nonblocking. we do not
			// want to block on a program receiving data. Note
			// that there is a small gotcha with it. More data is
			// queued when more data is read from the program
			// output. Hence, when having a program in
			// mode_manual where the first write cannot write
			// everything, the rest will be stuck in a queue that
			// is never emptied.
			if ( ! SetFDFlags(pipes[stdin_out], F_SETFL, O_NONBLOCK) )
				return false;
			}
		else
			ClosePipeEnd(stdin_out);

		ClosePipeEnd(stderr_out);

		if ( use_stderr )
			{
			if ( ! SetFDFlags(pipes[stderr_in], F_SETFL, O_NONBLOCK) )
				return false;
			}
		else
			ClosePipeEnd(stderr_in);

		file = fdopen(pipes[stdout_in], "r");

		if ( ! file )
			{
			Error("Could not convert stdout_in fileno to file");
			return false;
			}

		pipes[stdout_in] = -1; // will be closed by fclose

		if ( use_stderr )
			{
			stderrfile = fdopen(pipes[stderr_in], "r");

			if ( ! stderrfile )
				{
				Error("Could not convert stderr_in fileno to file");
				return false;
				}

			pipes[stderr_in] = -1; // will be closed by fclose
			}

		return true;
		}
	}

bool Raw::OpenInput()
	{
	if ( execute )
		return Execute();

	else
		{
		file = fopen(fname.c_str(), "r");
		if ( ! file )
			{
			Error(Fmt("Init: cannot open %s", fname.c_str()));
			return false;
			}

		fcntl(fileno(file),  F_SETFD, FD_CLOEXEC);
		}

	return true;
	}

bool Raw::CloseInput()
	{
	if ( file == 0 )
		{
		InternalError(Fmt("Trying to close closed file for stream %s", fname.c_str()));
		return false;
		}
#ifdef DEBUG
	Debug(DBG_INPUT, "Raw reader starting close");
#endif

	fclose(file);

	if ( use_stderr )
		fclose(stderrfile);

	if ( execute )
		{
		for ( int i = 0; i < 6; i ++ )
			ClosePipeEnd(i);
		}

	file = 0;
	stderrfile = 0;

#ifdef DEBUG
	Debug(DBG_INPUT, "Raw reader finished close");
#endif

	return true;
	}

bool Raw::DoInit(const ReaderInfo& info, int num_fields, const Field* const* fields)
	{
	if ( ! info.source || strlen(info.source) == 0 )
		{
		Error("No source path provided");
		return false;
		}

	fname = info.source;
	mtime = 0;
	execute = false;
	firstrun = true;
	int want_fields = 1;
	bool result;

	string source = string(info.source);
	char last = info.source[source.length() - 1];
	if ( last == '|' )
		{
		execute = true;
		fname = source.substr(0, fname.length() - 1);
		}

	map<const char*, const char*>::const_iterator it = info.config.find("stdin"); // data that is sent to the child process
	if ( it != info.config.end() )
		{
		stdin_string = it->second;
		stdin_towrite = stdin_string.length();
		}

	it = info.config.find("read_stderr"); // we want to read stderr
	if ( it != info.config.end() && execute )
		{
		use_stderr = true;
		want_fields = 2;
		}

	it = info.config.find("force_kill"); // we want to be sure that our child is dead when we exit
	if ( it != info.config.end() && execute )
		{
		forcekill = true;
		}

	if ( num_fields != want_fields )
		{
		Error(Fmt("Filter for raw reader contains wrong number of fields -- got %d, expected %d. "
		      "Filters for the raw reader contain one string field when used in normal mode and one string and one bool fields when using execute mode with stderr capuring. "
		      "Filter ignored.", num_fields, want_fields));
		return false;
		}

	if ( fields[0]->type != TYPE_STRING )
		{
		Error("First field for raw reader always has to be of type string.");
		return false;
		}
	if ( use_stderr && fields[1]->type != TYPE_BOOL )
		{
		Error("Second field for raw reader always has to be of type bool.");
		return false;
		}

	if ( execute && Info().mode == MODE_REREAD )
		{
		// for execs this makes no sense - would have to execute each heartbeat?
		Error("Rereading only supported for files, not for executables.");
		return false;
		}


	result = OpenInput();

	if ( result == false )
		return result;

#ifdef DEBUG
	Debug(DBG_INPUT, "Raw reader created, will perform first update");
#endif

	// after initialization - do update
	DoUpdate();

#ifdef DEBUG
	Debug(DBG_INPUT, "First update went through");
#endif
	return true;
	}

int64_t Raw::GetLine(FILE* arg_file)
	{
	errno = 0;
	int pos = 0; // strstr_n only works on ints - so no use to use something different here
	int offset = 0;

	if ( buf == 0 )
		buf = new char[block_size];

	int repeats = 1;

	for ( ;; )
		{
		size_t readbytes = fread(buf+bufpos+offset, 1, block_size-bufpos, arg_file);
		pos += bufpos + readbytes;
		//printf("Pos: %d\n", pos);
		bufpos = offset = 0; // read full block size in next read...

		if ( pos == 0 && errno != 0 )
			break;

		// researching everything each time is a bit... cpu-intensive. But otherwhise we have
		// to deal with situations where the separator is multi-character and split over multiple
		// reads...
		int found = strstr_n(pos, (unsigned char*) buf, separator.size(), (unsigned char*) separator.c_str());

		if ( found == -1 )
			{
			// we did not find it and have to search again in the next try. resize buffer....
			// but first check if we encountered the file end - because if we did this was it.
			if ( feof(arg_file) != 0 )
				{
				if ( pos == 0 )
					return -1; // signal EOF - and that we had no more data.
				else
					{
					outbuf = buf;
					buf = 0;
					return pos;
					}
				}

			repeats++;
			// bah, we cannot use realloc because we would have to change the delete in the manager to a free.
			char * newbuf = new char[block_size*repeats];
			memcpy(newbuf, buf, block_size*(repeats-1));
			delete [] buf;
			buf = newbuf;
			offset = block_size*(repeats-1);
			}
		else
			{
			outbuf = buf;
			buf = 0;

			if ( found < pos )
				{
				// we have leftovers. copy them into the buffer for the next line
				buf = new char[block_size];
				memcpy(buf, outbuf + found + sep_length, pos - found - sep_length);
				bufpos = pos - found - sep_length;
				}

			return found;
			}

		}

	if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
		return -2;

	else
		{
		// an error code we did no expect. This probably is bad.
		Error(Fmt("Reader encountered unexpected error code %d", errno));
		return -3;
		}

	InternalError("Internal control flow execution error in raw reader");
	assert(false);
	}

// write to the stdin of the child process
void Raw::WriteToStdin()
	{
	assert(stdin_towrite <= stdin_string.length());
	uint64_t pos = stdin_string.length() - stdin_towrite;

	errno = 0;
	ssize_t written = write(pipes[stdin_out], stdin_string.c_str() + pos, stdin_towrite);
	stdin_towrite -= written;

	if ( errno != 0 && errno != EAGAIN && errno != EWOULDBLOCK )
		{
		Error(Fmt("Writing to child process stdin failed: %d. Stopping writing at position %d", errno, pos));
		stdin_towrite = 0;
		}

	if ( stdin_towrite == 0 ) // send EOF when we are done.
		ClosePipeEnd(stdin_out);

	if ( Info().mode == MODE_MANUAL && stdin_towrite != 0 )
		{
		Error(Fmt("Could not write whole string to stdin of child process in one go. Please use STREAM mode to pass more data to child."));
		}
	}


// read the entire file and send appropriate thingies back to InputMgr
bool Raw::DoUpdate()
	{
	if ( firstrun )
		firstrun = false;

	else
		{
		switch ( Info().mode  ) {
		case MODE_REREAD:
			{
			assert(childpid == -1); // mode may not be used to execute child programs
			// check if the file has changed
			struct stat sb;
			if ( stat(fname.c_str(), &sb) == -1 )
				{
				Error(Fmt("Could not get stat for %s", fname.c_str()));
				return false;
				}

			if ( sb.st_mtime <= mtime )
				// no change
				return true;

			mtime = sb.st_mtime;
			// file changed. reread.
			//
			// fallthrough
			}

		case MODE_MANUAL:
		case MODE_STREAM:
			if ( Info().mode == MODE_STREAM && file != 0 )
				{
				clearerr(file);  // remove end of file evil bits
				break;
				}

			CloseInput();
			if ( ! OpenInput() )
				return false;

			break;

		default:
			assert(false);
		}
		}

	string line;
	assert ( (NumFields() == 1 && !use_stderr) || (NumFields() == 2 && use_stderr));
	for ( ;; )
		{
		if ( stdin_towrite > 0 )
			WriteToStdin();

		int64_t length = GetLine(file);
		//printf("Read %lld bytes\n", length);

		if ( length == -3 )
			return false;

		else if ( length == -2 || length == -1 )
			// no data ready or eof
			break;

		Value** fields = new Value*[2]; // just always reserve 2. This means that our [] is too long by a count of 1 if not using stderr. But who cares...

		// filter has exactly one text field. convert to it.
		Value* val = new Value(TYPE_STRING, true);
		val->val.string_val.data = outbuf;
		val->val.string_val.length = length;
		fields[0] = val;

		if ( use_stderr )
			{
			Value* bval = new Value(TYPE_BOOL, true);
			bval->val.int_val = 0;
			fields[1] = bval;
			}

		Put(fields);

		outbuf = 0;
		}

	if ( use_stderr )
		{
		for ( ;; )
			{
			int64_t length = GetLine(stderrfile);
			//printf("Read stderr %lld bytes\n", length);
			if ( length == -3 )
				return false;

			else if ( length == -2 || length == -1 )
				break;

			Value** fields = new Value*[2];
			Value* val = new Value(TYPE_STRING, true);
			val->val.string_val.data = outbuf;
			val->val.string_val.length = length;
			fields[0] = val;
			Value* bval = new Value(TYPE_BOOL, true);
			bval->val.int_val = 1; // yes, we are stderr
			fields[1] = bval;

			Put(fields);

			outbuf = 0;
			}
		}

	if ( ( Info().mode == MODE_MANUAL ) || ( Info().mode == MODE_REREAD ) )
		// done with the current data source
		EndCurrentSend();

	// and let's check if the child process is still alive
	int return_code;
	if ( childpid != -1 && waitpid(childpid, &return_code, WNOHANG) != 0 )
		{
		// child died
		childpid = -1;
		bool signal = false;
		int code = 0;
		if ( WIFEXITED(return_code) )
			{
			code = WEXITSTATUS(return_code);
			if ( code != 0 )
				Error(Fmt("Child process exited with non-zero return code %d", code));
			}

		else if ( WIFSIGNALED(return_code) )
			{
			signal = true;
			code = WTERMSIG(return_code);
			Error(Fmt("Child process exited due to signal %d", code));
			}

		else
			assert(false);

		Value** vals = new Value*[4];
		vals[0] = new Value(TYPE_STRING, true);
		vals[0]->val.string_val.data = copy_string(Info().name);
		vals[0]->val.string_val.length = strlen(Info().name);
		vals[1] = new Value(TYPE_STRING, true);
		vals[1]->val.string_val.data = copy_string(Info().source);
		vals[1]->val.string_val.length = strlen(Info().source);
		vals[2] = new Value(TYPE_COUNT, true);
		vals[2]->val.int_val = code;
		vals[3] = new Value(TYPE_BOOL, true);
		vals[3]->val.int_val = signal;

		// and in this case we can signal end_of_data even for the streaming reader
		if ( Info().mode == MODE_STREAM )
			EndCurrentSend();

		SendEvent("InputRaw::process_finished", 4, vals);
		}



#ifdef DEBUG
	Debug(DBG_INPUT, "DoUpdate finished successfully");
#endif

	return true;
	}

bool Raw::DoHeartbeat(double network_time, double current_time)
	{
	switch ( Info().mode ) {
		case MODE_MANUAL:
			// yay, we do nothing :)
			break;

		case MODE_REREAD:
		case MODE_STREAM:
#ifdef DEBUG
	Debug(DBG_INPUT, "Starting Heartbeat update");
#endif
			Update();	// call update and not DoUpdate, because update
					// checks disabled.
#ifdef DEBUG
	Debug(DBG_INPUT, "Finished with heartbeat update");
#endif
			break;
		default:
			assert(false);
	}

	return true;
	}
