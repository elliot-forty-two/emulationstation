#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifdef WIN32
#include <windows.h>
#endif

// Publishes EmulationStation selection changes over a local Windows named pipe.
//
// Pipe:
//   \\.\pipe\EmulationStation.Events
//
// Format:
//   One UTF-8 JSON object per line (NDJSON).
//
// Example:
//   {"event":"game-selected","sequence":42,"type":"game",
//    "system":"vpinball","path":"C:\\...\\table.vpx","name":"Table"}\n
class NamedPipeEventBroadcaster
{
public:
	static NamedPipeEventBroadcaster& getInstance()
	{
		static NamedPipeEventBroadcaster instance;
		return instance;
	}

    void publishEvent(const std::string& eventName, const std::string& arg1, const std::string& arg2, const std::string& arg3)
    {
		std::lock_guard<std::mutex> lock(mMutex);

		std::string arg1Name;
		std::string arg2Name;
		std::string arg3Name;

		if (eventName == "system-selected")
		{
			arg1Name = "system";
		}
		else if (eventName == "game-selected")
		{
			arg1Name = "system";
			arg2Name = "path";
			arg3Name = "name";
		}
		else if (eventName == "game-start")
		{
			arg1Name = "path";
			arg2Name = "basename";
			arg3Name = "name";
		}
		else if (eventName == "screensaver-start")
		{
			arg1Name = "behavior";
		}
		else if (eventName == "theme-changed")
		{
			arg1Name = "newTheme";
			arg2Name = "oldTheme";
		}
		else if (eventName == "quit")
		{
			arg1Name = "action";
		}

		const std::uint64_t sequence = ++mEventSequence;

		std::ostringstream json;
		json << "{";
		json << "\"event\":\"" << eventName << "\",";
		if (!arg1Name.empty())
			json << "\"" << arg1Name << "\":\"" << escapeJson(arg1) << "\",";
		if (!arg2Name.empty())
			json << "\"" << arg2Name << "\":\"" << escapeJson(arg2) << "\",";
		if (!arg3Name.empty())
			json << "\"" << arg3Name << "\":\"" << escapeJson(arg3) << "\",";
		json << "\"sequence\":" << sequence;
		json << "}\n";
		
		if (eventName == "game-selected")
		{
			++mGeneration;
			mLatestPayload = json.str();
			mCondition.notify_one();
		}
		else
		{	 
			mControlEvents.push_back(json.str());
			mCondition.notify_one();
		}
    }

private:
	NamedPipeEventBroadcaster()
		: mStop(false),
		  mGeneration(0),
		  mEventSequence(0)
	{
#ifdef WIN32
		mWorker = std::thread(&NamedPipeEventBroadcaster::run, this);
#endif
	}

	~NamedPipeEventBroadcaster()
	{
#ifdef WIN32
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mStop = true;
		}

		mCondition.notify_all();

		if (mWorker.joinable())
			mWorker.join();
#endif
	}

	NamedPipeEventBroadcaster(const NamedPipeEventBroadcaster&) = delete;
	NamedPipeEventBroadcaster& operator=(const NamedPipeEventBroadcaster&) = delete;

	static std::string escapeJson(const std::string& value)
	{
		std::ostringstream out;

		for (unsigned char c : value)
		{
			switch (c)
			{
			case '"':
				out << "\\\"";
				break;
			case '\\':
				out << "\\\\";
				break;
			case '\b':
				out << "\\b";
				break;
			case '\f':
				out << "\\f";
				break;
			case '\n':
				out << "\\n";
				break;
			case '\r':
				out << "\\r";
				break;
			case '\t':
				out << "\\t";
				break;
			default:
				if (c < 0x20)
				{
					out << "\\u"
						<< std::hex << std::setw(4) << std::setfill('0')
						<< static_cast<int>(c)
						<< std::dec << std::setfill(' ');
				}
				else
					out << static_cast<char>(c);
				break;
			}
		}

		return out.str();
	}

#ifdef WIN32
	void run()
	{
		static const char* PIPE_NAME = "\\\\.\\pipe\\EmulationStation.Events";

		while (!shouldStop())
		{
			HANDLE pipe = CreateNamedPipeA(
				PIPE_NAME,
				PIPE_ACCESS_OUTBOUND,
				PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
				1,
				64 * 1024,
				0,
				0,
				nullptr);

			if (pipe == INVALID_HANDLE_VALUE)
			{
				waitForRetry();
				continue;
			}

			bool connected = false;

			while (!shouldStop() && !connected)
			{
				if (ConnectNamedPipe(pipe, nullptr))
				{
					connected = true;
					break;
				}

				const DWORD error = GetLastError();

				if (error == ERROR_PIPE_CONNECTED)
				{
					connected = true;
					break;
				}

				if (error != ERROR_PIPE_LISTENING && error != ERROR_NO_DATA)
					break;

				waitForRetry();
			}

			if (connected)
			{
				// Reset for each connection so a newly connected viewer is
				// immediately given the latest known selection.
				std::uint64_t sentGeneration = 0;

				while (!shouldStop())
				{
					std::string payload;
					std::uint64_t generation = sentGeneration;
					bool controlEvent = false;

					{
						std::unique_lock<std::mutex> lock(mMutex);

						mCondition.wait(lock, [&]()
							{
								return mStop
									|| !mControlEvents.empty()
									|| mGeneration != sentGeneration;
							});

						if (mStop)
							break;

						// Control events are guaranteed delivery and take priority.
						if (!mControlEvents.empty())
						{
							payload = std::move(mControlEvents.front());
							mControlEvents.pop_front();
							controlEvent = true;
						}
						else
						{
							// Selection events remain latest-state-wins.
							payload = mLatestPayload;
							generation = mGeneration;
						}
					}

					if (payload.empty())
					{
						if (!controlEvent)
							sentGeneration = generation;
						continue;
					}

					DWORD bytesWritten = 0;
					const BOOL ok = WriteFile(
						pipe,
						payload.data(),
						static_cast<DWORD>(payload.size()),
						&bytesWritten,
						nullptr);

					if (!ok || bytesWritten != payload.size())
						break;

					if (!controlEvent)
						sentGeneration = generation;
				}
			}

			DisconnectNamedPipe(pipe);
			CloseHandle(pipe);
		}
	}

	bool shouldStop()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return mStop;
	}

	void waitForRetry()
	{
		std::unique_lock<std::mutex> lock(mMutex);
		mCondition.wait_for(lock, std::chrono::milliseconds(25), [&]()
			{
				return mStop;
			});
	}

	std::thread mWorker;
#endif

	std::mutex mMutex;
	std::condition_variable mCondition;
	bool mStop;
	std::uint64_t mGeneration;
	std::uint64_t mEventSequence;
	std::deque<std::string> mControlEvents;
	std::string mLatestPayload;
};