/*
 * @file  filesystem-lru-cache.cc
 * @brief additional implementation of file system LRU cache
 *
 * @date Nov 20, 2014
 * @author elenav
 */

#include <map>
#include <boost/regex.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>

#include "dfs_cache/cache-mgr.hpp"
#include "dfs_cache/filesystem-lru-cache.hpp"
#include "dfs_cache/utilities.hpp"

namespace impala{

bool FileSystemLRUCache::deleteFile(managed_file::File* file, bool physically){
	// no matter the scenario, do not pass to removal if any clients still use the file:
	if(!isItemIdle(file))
		return false;

	// no usage so far, mark the file for deletion:
	file->state(managed_file::State::FILE_IS_MARKED_FOR_DELETION);

	// for physical removal scenario, drop the file from file system
	if (physically) {
		// delegate further deletion scenario to the file itself:
		file->drop();
	}

	// get rid of file metadata object:
	delete file;
	return true;
}

void FileSystemLRUCache::continuationFor(managed_file::File* file){
	// if file is not valid, break the handler
	if (file == nullptr || !file->valid())
		return;

	// mark file as "in progress":
	file->state(managed_file::State::FILE_IS_IN_USE_BY_SYNC);

	// and wait for prepare operation will be finished:
	DataSet data;
	data.push_back(file->relative_name().c_str());

	bool condition = false;
	boost::condition_variable condition_var;
	boost::mutex completion_mux;

	status::StatusInternal cbStatus;

	PrepareCompletedCallback cb =
			[&] (SessionContext context,
					const std::list<boost::shared_ptr<FileProgress> > & progress,
					request_performance const & performance, bool overall,
					bool canceled, taskOverallStatus status) -> void {

				boost::lock_guard<boost::mutex> lock(completion_mux);
				cbStatus = (status == taskOverallStatus::COMPLETED_OK ? status::StatusInternal::OK : status::StatusInternal::REQUEST_FAILED);
				if(status != taskOverallStatus::COMPLETED_OK) {
					LOG (ERROR) << "Failed to load file \"" << file->fqp() << "\"" << ". Status : "
					<< status << ".\n";
					file->state(managed_file::State::FILE_IS_FORBIDDEN);
				}
				if(context == NULL)
				LOG (ERROR) << "NULL context received while loading the file \"" << file->fqp()
				<< "\"" << ".Status : " << status << ".\n";

				if(progress.size() != data.size())
				LOG (ERROR) << "Expected amount of progress is not equal to received for file \""
				<< file->fqp() << "\"" << ".Status : " << status << ".\n";

				if(!overall)
				LOG (ERROR) << "Expected amount of progress is not equal to received for file \""
				<< file->fqp() << "\"" << ".Status : " << status << ".\n";

				condition = true;
				condition_var.notify_all();
			};

	requestIdentity identity;

	auto f1 = std::bind(&CacheManager::cachePrepareData,
			CacheManager::instance(), ph::_1, ph::_2, ph::_3, ph::_4, ph::_5);

	boost::uuids::uuid uuid = boost::uuids::random_generator()();

	std::string local_client = boost::lexical_cast<std::string>(uuid);
	SessionContext ctx = static_cast<void*>(&local_client);

	status::StatusInternal status;

	FileSystemDescriptor fsDescriptor;
	fsDescriptor.dfs_type = file->origin();
	fsDescriptor.host = file->host();
	try {
		fsDescriptor.port = std::stoi(file->port());
	} catch (...) {
		return;
	}
	// execute request in async way to utilize requests pool:
	status = f1(ctx, std::cref(fsDescriptor), std::cref(data), cb,
			std::ref(identity));

	// check operation scheduling status:
	if (status != status::StatusInternal::OPERATION_ASYNC_SCHEDULED) {
		LOG (ERROR)<< "Prepare request - failed to schedule - for \"" << file->fqnp() << "\"" << ". Status : "
		<< status << ".\n";
		// no need to wait for callback to fire, operation was not scheduled
		return;
	}

	// wait when completion callback will be fired by Prepare scenario:
	boost::unique_lock<boost::mutex> lock(completion_mux);
	condition_var.wait(lock, [&] {return condition;});

	lock.unlock();

	// check callback status:
	if (cbStatus != status::StatusInternal::OK) {
		LOG (ERROR)<< "Prepare request failed for \"" << file->fqnp() << "\"" << ". Status : "
		<< status << ".\n";
		file->state(managed_file::State::FILE_IS_FORBIDDEN);
		return;
	}
	else
	// file is present and is ready to use
	file->state(managed_file::State::FILE_IS_IDLE);
}

bool FileSystemLRUCache::reload(const std::string& root){
	if(root.empty())
		return false;
	m_root = root;

	boost::filesystem::recursive_directory_iterator end_iter;

	// sort files in the root in ascending order basing on their timestamp:
	typedef std::multimap<std::time_t, boost::filesystem::path> last_access_multi;
	// iterator for sorted collection:
	typedef std::multimap<std::time_t, boost::filesystem::path>::iterator last_access_multi_it;
	last_access_multi result_set;

	// note that std::time_t accurate to a second
	if ( boost::filesystem::exists(m_root) && boost::filesystem::is_directory(m_root)){
	  for( boost::filesystem::recursive_directory_iterator dir_iter(m_root) ; dir_iter != end_iter ; ++dir_iter){
	    if (boost::filesystem::is_regular_file(dir_iter->status()) ){
	    	result_set.insert(last_access_multi::value_type(boost::filesystem::last_write_time(dir_iter->path()), *dir_iter));
	    }
	  }
	}

	// reset the underlying LRU cache.
	reset();
	last_access_multi_it it = result_set.begin();
	// reload most old timestamp:
	m_startTime = boost::posix_time::from_time_t((*it).first);

	// and populate sorted root content:
    for(; it != result_set.end(); it++){
    	std::string lp = (*it).second.string();
    	// create the managed file instance if there's network path can be successfully restored from its name
    	// so that the file can be managed by Imapla-To-Go:
    	std::string fqnp;
    	std::string relative;
        FileSystemDescriptor desciptor = managed_file::File::restoreNetworkPathFromLocal(lp, fqnp, relative);
        if(!desciptor.valid)
        	continue; // do not register this file

        managed_file::File* file;
    	// and add it into the cache
    	add(lp, file);
    	// and mark the file as "idle":
    	file->state(managed_file::State::FILE_IS_IDLE);
    }
    return true;
}

}



