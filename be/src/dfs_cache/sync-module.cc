/*
 * @file sync-module.cpp
 * @brief implementation of sync module
 *
 *  Created on: Oct 3, 2014
 *      Author: elenav
 */

/**
 * @namespace impala
 */

#include "dfs_cache/sync-module.hpp"
#include "dfs_cache/dfs-connection.hpp"

namespace impala {

status::StatusInternal Sync::estimateTimeToGetFileLocally(const NameNodeDescriptor & namenode, const char* path,
		request::MakeProgressTask<boost::shared_ptr<FileProgress> >* const & task){

	// Get the Namenode adaptor from the registry for requested namenode:
	boost::shared_ptr<NameNodeDescriptorBound> namenodeAdaptor = (*m_registry->getNamenode(namenode));
    if(namenodeAdaptor == nullptr){
    	// no namenode adaptor configured, go out
    	return status::StatusInternal::NAMENODE_IS_NOT_CONFIGURED;
    }
    raiiDfsConnection connection(namenodeAdaptor->getFreeConnection());
    if(!connection.valid()) {
    	LOG (ERROR) << "No connection to dfs available, no estimate actions will be taken for namenode \"" << namenode.dfs_type << ":" <<
    			namenode.host << "\"" << "\n";
    	return status::StatusInternal::DFS_NAMENODE_IS_NOT_REACHABLE;
    }

    boost::shared_ptr<RemoteAdaptor> adaptor    = namenodeAdaptor->adaptor();

    // Execute the remote estimation operation on the adaptor.
    // wait for execution.
    // free the connection so it is available for further usage

    // get the file progress reference:
    boost::shared_ptr<FileProgress> fp = task->progress();

    // set the progress directly to the task
	return status::StatusInternal::OK;
}

status::StatusInternal Sync::prepareFile(const NameNodeDescriptor & namenode, const char* path,
		request::MakeProgressTask<boost::shared_ptr<FileProgress> >* const & task){
	// Get the Namenode adaptor from the registry for requested namenode:
	boost::shared_ptr<NameNodeDescriptorBound> namenodeAdaptor = (*m_registry->getNamenode(namenode));
    if(namenodeAdaptor == nullptr){
    	// no namenode adaptor configured, go out
    	return status::StatusInternal::NAMENODE_IS_NOT_CONFIGURED;
    }

    raiiDfsConnection connection(namenodeAdaptor->getFreeConnection());
    if(!connection.valid()) {
    	LOG (ERROR) << "No connection to dfs available, no prepare actions will be taken for namenode \"" << namenode.dfs_type << ":" <<
    			namenode.host << "\"" << "\n";
    	return status::StatusInternal::DFS_NAMENODE_IS_NOT_REACHABLE;
    }

    boost::shared_ptr<RemoteAdaptor> adaptor = namenodeAdaptor->adaptor();

    // get the file progress reference:
    boost::shared_ptr<FileProgress> fp = task->progress();
    adaptor->read(connection.connection());


    // Pure academic part.
    //
	// Suppose download in progress.
	int bytes_read = 0;
	// while no cancellation and we still something to read, proceed.
	boost::mutex* mux;
	boost::condition_variable_any* conditionvar;
	task->mux(mux);
	task->conditionvar(conditionvar);

	while(!task->condition() && bytes_read != 0){
		boost::mutex::scoped_lock lock(*mux);
		// do read a block
		bytes_read = 10;
		lock.unlock();
	}
	// check cancellation condition to know we had the cancellation. If so, notify the caller (no matter if it waits for this or no):
	conditionvar->notify_all();

	return status::StatusInternal::OK;
}

status::StatusInternal Sync::cancelFileMakeProgress(bool async, request::CancellableTask* const & task){
	boost::mutex* mux;
	boost::condition_variable_any* conditionvar;
	task->mux(mux);
	task->conditionvar(conditionvar);

	boost::mutex::scoped_lock lock(*mux);
	// set the cancellation flag
	bool* condition;
	task->condition(condition);
    *condition = true;

	if(!async){
		conditionvar->wait(*mux);
		// update the history!1
	} // else return immediately
	return status::StatusInternal::OK;
}

status::StatusInternal Sync::validateLocalCache(bool& valid){
	return status::StatusInternal::NOT_IMPLEMENTED;
}
}


