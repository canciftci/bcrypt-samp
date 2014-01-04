#include <thread>

#include "plugin.h"
#include "bcrypt.h"

plugin *plugin::instance = NULL;

plugin::plugin()
{

}

plugin::~plugin()
{
	this->logprintf("plugin.bcrypt: Plugin unloaded.");
}

void plugin::initialise(void **ppData)
{
	instance = new plugin();

	instance->logprintf = (logprintf_t) ppData[samp_sdk::PLUGIN_DATA_LOGPRINTF];

	int threads_supported = std::thread::hardware_concurrency();
	instance->thread_limit = threads_supported - 1;

	if (instance->thread_limit < 1)
		instance->thread_limit = 1;

	instance->logprintf("  plugin.bcrypt "BCRYPT_VERSION" was loaded.");
	instance->logprintf("  plugin.bcrypt: %d cores detected, %d concurrent threads will be used.", threads_supported, instance->thread_limit);
}

plugin *plugin::get()
{
	return instance;
}

void plugin::add_amx(samp_sdk::AMX *amx)
{
	get()->amx_list.insert(amx);
}

void plugin::remove_amx(samp_sdk::AMX *amx)
{
	get()->amx_list.erase(amx);
}

void plugin::set_thread_limit(int value)
{
	this->thread_limit = value;
}

int plugin::get_thread_limit()
{
	return this->thread_limit;
}

void plugin::queue_task(unsigned short type, int thread_idx, int thread_id, std::string key, unsigned short cost)
{
	this->task_queue.push({ type, thread_idx, thread_id, key, cost, "" });
}

void plugin::queue_task(unsigned short type, int thread_idx, int thread_id, std::string key, std::string hash)
{
	this->task_queue.push({ type, thread_idx, thread_id, key, NULL, hash});
}

void plugin::queue_result(unsigned short type, int thread_idx, int thread_id, std::string hash, bool match)
{
	std::lock_guard<std::mutex> lock(plugin::result_queue_mutex);

	this->result_queue.push_back({ type, thread_idx, thread_id, hash, match });
	this->active_threads--;
}

void thread_generate_bcrypt(int thread_idx, int thread_id, std::string buffer, short cost)
{
	bcrypt *crypter = new bcrypt();

	crypter
		->setCost(cost)
		->setPrefix("2y")
		->setKey(buffer);

	std::string hash = crypter->generate();

	delete(crypter);

	// Add the result to the queue
	plugin::get()->queue_result(E_QUEUE_HASH, thread_idx, thread_id, hash, false);
}

void thread_check_bcrypt(int thread_idx, int thread_id, std::string password, std::string hash)
{
	bool match;
	match = bcrypt::compare(password, hash);

	// Add the result to the queue
	plugin::get()->queue_result(E_QUEUE_CHECK, thread_idx, thread_id, "", match);
}

void plugin::process_task_queue()
{
	while (!this->task_queue.empty())
	{
		if (this->active_threads < this->thread_limit)
		{
			switch (this->task_queue.front().type)
			{
			case E_QUEUE_HASH:
			{
								 // Start a new thread
								 this->active_threads++;

								 std::thread t(thread_generate_bcrypt, this->task_queue.front().thread_idx, this->task_queue.front().thread_id, this->task_queue.front().key, this->task_queue.front().cost);
								 t.detach();

								 this->task_queue.pop();
								 break;
			}
			case E_QUEUE_CHECK:
			{
								  // Start a new thread
								  this->active_threads++;

								  std::thread t(thread_check_bcrypt, this->task_queue.front().thread_idx, this->task_queue.front().thread_id, this->task_queue.front().key, this->task_queue.front().hash);
								  t.detach();

								  this->task_queue.pop();
								  break;
			}

			default:
				break;
			}
		}
		else
		{
			break;
		}
	}
}

void plugin::process_result_queue()
{
	using namespace samp_sdk;

	if (this->result_queue.size() > 0)
	{
		std::lock_guard<std::mutex> lock(plugin::result_queue_mutex);

		int amx_idx;
		for (std::set<AMX *>::iterator a = this->amx_list.begin(); a != this->amx_list.end(); ++a)
		{
			for (std::vector<s_result_queue>::iterator t = this->result_queue.begin(); t != this->result_queue.end(); ++t)
			{
				if ((*t).type == E_QUEUE_HASH)
				{
					// public OnBcryptHashed(thread_idx, thread_id, const hash[]);

					if (!amx_FindPublic(*a, "OnBcryptHashed", &amx_idx))
					{
						// Push the hash
						cell addr;
						amx_PushString(*a, &addr, NULL, (*t).hash.c_str(), NULL, NULL);

						// Push the thread_id and thread_idx
						amx_Push(*a, (*t).thread_id);
						amx_Push(*a, (*t).thread_idx);

						// Execute and release memory
						amx_Exec(*a, NULL, amx_idx);
						amx_Release(*a, addr);
					}
				}
				else if ((*t).type == E_QUEUE_CHECK)
				{
					// public OnBcryptChecked(thread_idx, thread_id, bool:match);

					if (!amx_FindPublic(*a, "OnBcryptChecked", &amx_idx))
					{
						// Push the thread_id and thread_idx
						amx_Push(*a, (*t).match);
						amx_Push(*a, (*t).thread_id);
						amx_Push(*a, (*t).thread_idx);

						// Execute and release memory
						amx_Exec(*a, NULL, amx_idx);
					}
				}
			}
		}

		// Clear the queue
		this->result_queue.clear();
	}
}