#!/usr/bin/env python3
#
# Copyright (c) 2021 Versity Software, Inc. All rights reserved.
#

import os
import sys
import pwd
import grp
import time
import click
import pathlib
import confuse
import logging
import elastic
import concurrent.futures

import util as cliutil

from queue import Queue
from datetime import datetime
from threading import Lock, current_thread
from elasticsearch.helpers.errors import BulkIndexError

quit = False
emptyindex = False
warnings = 0
scan_paths = []
sizes = {}
gid_name = {}
uid_name = {}
bulktime = {}
dircount = {}
filecount = {}
inodecount = {}
skipdircount = {}
skipfilecount = {}
total_documents = {}

fileperm_lock = Lock()
crawl_queue = Queue()
crawl_lock = Lock()

maxthreads = int(os.cpu_count())

"""Load yaml config file."""
config = confuse.Configuration('scoutfs', __name__)

try:
    blocksize = config['scoutfs']['blocksize'].get()
    elastic_chunksize = config['databases']['elasticsearch']['chunksize'].get()
except confuse.NotFoundError as e:
    print('Config ERROR: {0}, check config for errors or missing settings from default config.'.format(e))
    sys.exit(1)

logger = logging.getLogger("crawl")
logformat = '%(asctime)s - %(name)s - %(levelname)s - %(message)s'
logging.basicConfig(format=logformat, level=logging.INFO)


def exit_app(es, index):
    global quit
    global emptyindex

    if quit:
        return
    quit = True
    crawl_queue.join()
    if not emptyindex:
        elastic.tune_index(es, index, defaults=True)
    if warnings > 0:
        sys.exit(78)
    sys.exit(0)


def exit_app_critical_error():
    os._exit(1)


def begin_bulk(es, index, thread, root, documents):
    global bulktime
    
    elastic_upload_starttime = time.time()
    try:
        elastic.bulk_upload(es, index, documents)
    except BulkIndexError as e:
        logger.critical('[{0}] FATAL ERROR: Elasticsearch BulkIndexError ({1})'.format(thread, e), exc_info=True)
        exit_app_critical_error()
    else:
        elastic_upload_time = time.time() - elastic_upload_starttime
        with crawl_lock:
            bulktime[root] += elastic_upload_time


def get_file_name(file, ignore_errors=False):
    return cliutil.handle_unicode(file, ignore_errors=ignore_errors)


def get_dir_name(path, ignore_errors=False):
    path = os.path.basename(path)
    return cliutil.handle_unicode(path, ignore_errors=ignore_errors)


def get_parent_path(path, ignore_errors=False):
    path = os.path.dirname(path)
    return cliutil.handle_unicode(path, ignore_errors=ignore_errors)


def get_owner_name(uid):
    global uid_name

    owner = None
    if uid in uid_name:
        owner = uid_name[uid]
    if owner is None:
        try:
            owner = pwd.getpwuid(uid).pw_name
        except Exception:
            owner = uid
        with fileperm_lock:
            uid_name[uid] = owner
    return owner


def get_group_name(gid):
    global gid_name

    group = None
    if gid in gid_name:
        group = gid_name[gid]
    if group is None:
        try:
            group = grp.getgrgid(gid).gr_name
        except Exception:
            group = gid
        with fileperm_lock:
            gid_name[gid] = group
    return group


def get_tree_attr(es, index, thread, root, top, path, documents, depth=0, maxdepth=999):
    global sizes
    global warnings
    global dircount
    global filecount
    global inodecount
    global skipdircount
    global skipfilecount
    global total_documents

    size = 0
    dirs = 0
    files = 0
    size_du = 0
    f_count = 0
    d_count = 0
    total_docs = 0
    file_skip_count = 0
    dir_skip_count = 0
    parent_path = None

    try:
        dir_stat = os.stat(path)
    except OSError as e:
        logger.warning('[{0}] OS ERROR: {1}'.format(thread, e))
        with crawl_lock:
            warnings += 1
        return 0, 0, 0, 0

    try:
        for content in os.scandir(path):    
            if content.is_symlink():
                pass
            elif content.is_dir():
                d_count += 1
                dirs += 1
                if maxdepth > 0 and depth < maxdepth and not quit:
                    subdir_size, subdir_du, subdir_fc, subdir_dc = get_tree_attr(es, index, thread, root,
                                                                                 top, content.path, documents,
                                                                                 depth + 1, maxdepth)
                    size += subdir_size
                    size_du += subdir_du
                    files += subdir_fc
                    dirs += subdir_dc
            else:
                f_count += 1
                f_stat = content.stat()
                fsize = f_stat.st_size
                fsize_du = f_stat.st_blocks * blocksize

                if fsize > 0:
                    size += fsize
                    size_du += fsize_du
                    files += 1
                    owner = get_owner_name(f_stat.st_uid)
                    group = get_group_name(f_stat.st_gid)

                    try:
                        if parent_path is None:
                            parent_path = get_parent_path(content.path)
                        file_name = get_file_name(content.name)
                    except UnicodeError:
                        if parent_path is None:
                            parent_path = get_parent_path(content.path, ignore_errors=True)
                        file_name = get_file_name(content.name, ignore_errors=True)
                        logger.warning('[{0}] UNICODE WARNING {1}'.format(thread, os.path.join(parent_path, file_name)))
                        with crawl_lock:
                            warnings += 1
                        pass
                    data = {
                        'type': 'file',
                        'size': fsize,
                        'owner': owner,
                        'group': group,
                        'name': file_name,
                        'size_du': fsize_du,
                        'nlink': f_stat.st_nlink,
                        'ino': str(f_stat.st_ino),
                        'parent_path': parent_path,
                        'extension': pathlib.Path(content.name).suffix,
                        'mtime': datetime.utcfromtimestamp(int(f_stat.st_mtime)).isoformat(),
                        'atime': datetime.utcfromtimestamp(int(f_stat.st_atime)).isoformat(),
                        'ctime': datetime.utcfromtimestamp(int(f_stat.st_ctime)).isoformat()
                    }
                    documents.append(data.copy())
                    document_count = len(documents)
                    if document_count >= elastic_chunksize:
                        begin_bulk(es, index, thread, root, documents)
                        total_docs += document_count
                        documents.clear()
                else:
                    file_skip_count += 1

        if files > 0 or dirs > 0:
            owner = get_owner_name(dir_stat.st_uid)
            group = get_group_name(dir_stat.st_gid)

            try:
                file_name = get_dir_name(path)
                parent_path = get_parent_path(path)
            except UnicodeError:
                file_name = get_dir_name(path, ignore_errors=True)
                parent_path = get_parent_path(path, ignore_errors=True)
                logger.warning('[{0}] UNICODE WARNING {1}'.format(thread, os.path.join(parent_path, file_name)))
                with crawl_lock:
                    warnings += 1
                pass

            data = {
                'size': size,
                'owner': owner,
                'group': group,
                'name': file_name,
                'size_du': size_du,
                'file_count': files,
                'type': 'directory',
                'dir_count': dirs + 1,
                'nlink': dir_stat.st_nlink,
                'parent_path': parent_path,
                'ino': str(dir_stat.st_ino),
                'mtime': datetime.utcfromtimestamp(int(dir_stat.st_mtime)).isoformat(),
                'atime': datetime.utcfromtimestamp(int(dir_stat.st_atime)).isoformat(),
                'ctime': datetime.utcfromtimestamp(int(dir_stat.st_ctime)).isoformat()
                }
                    
            if depth > 0:
                documents.append(data.copy())
                document_count = len(documents)
                if document_count >= elastic_chunksize:
                    begin_bulk(es, index, thread, root, documents)
                    total_docs += document_count
                    documents.clear()  
            else:
                with crawl_lock:
                    sizes[root] = data.copy()
        else:
            dir_skip_count += 1
            logger.debug('[{0}] skipping empty dir {1}'.format(thread, path))
            if dirs > 0:
                dirs -= 1

        with crawl_lock:
            total_documents[root] += total_docs
            skipdircount[root] += dir_skip_count
            inodecount[root] += d_count + f_count
            skipfilecount[root] += file_skip_count
            dircount[root] += d_count - dir_skip_count
            filecount[root] += f_count - file_skip_count

    except OSError as e:
        logger.warning('[{0}] OS ERROR: {1}'.format(thread, e))
        with crawl_lock:
            warnings += 1
        pass
    
    return size, size_du, files, dirs


def crawl_thread(root, es, index, top, depth, maxdepth):
    global sizes
    global total_documents
    global scan_paths

    documents = []
    thread = current_thread().name
    crawl_start = time.time()

    with crawl_lock:
        scan_paths.append(top)
    size, size_du, file_count, dir_count = get_tree_attr(es, index, thread, root, top,
                                                         top, documents, depth, maxdepth)
    document_count = len(documents)
    if document_count > 0:
        begin_bulk(es, index, thread, root, documents)
        with crawl_lock:
            total_documents[root] += document_count
        documents.clear()
    # Add sizes of subdirectories to top directory (root)
    if depth > 0:
        with crawl_lock:
            sizes[top] = {
                'size': size,
                'size_du': size_du,
                'dir_count': dir_count,
                'file_count': file_count
            }
        if size > 0:
            with crawl_lock:
                sizes[root]['size'] += sizes[top]['size']
                sizes[root]['size_du'] += sizes[top]['size_du']
                sizes[root]['dir_count'] += sizes[top]['dir_count']
                sizes[root]['file_count'] += sizes[top]['file_count']
    
    crawl_time = cliutil.get_time(time.time() - crawl_start)
    logger.info('[{0}] finished crawling {1} ({2} dirs, {3} files, {4}) in {5}'.format(
            thread, top, dir_count, file_count, cliutil.convert_size(size), crawl_time))
    with crawl_lock:
        scan_paths.remove(top)


def crawl(root, es, index, maxdepth):
    global emptyindex

    scandir_walk_start = time.time()
    # find all subdirs on root to crawl async threads
    subdir_list = []
    for content in os.scandir(root):
        if content.is_symlink():
            pass
        elif content.is_dir():
            subdir_list.append(content.path)
        
    with concurrent.futures.ThreadPoolExecutor(max_workers=maxthreads) as executor:
        future = executor.submit(crawl_thread, root, es, index, root, 0, 0)
        try:
            data = future.result()
        except Exception as e:
            logger.critical('FATAL ERROR: an exception has occurred: {0}'.format(e), exc_info=True)
            exit_app_critical_error()

        futures_subdir = {executor.submit(crawl_thread, root, es, index, subdir, 1, maxdepth): subdir for subdir in subdir_list}
        for future in concurrent.futures.as_completed(futures_subdir):
            try:
                data = future.result()
            except Exception as e:          
                logger.critical('FATAL ERROR: an exception has occurred: {0}'.format(e), exc_info=True)
                exit_app_critical_error()

    scandir_walk_time = time.time() - scandir_walk_start
    end_time = datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%S")

    if not root in sizes:
        emptyindex = True
        logger.info('*** finished walking {0} ***'.format(root))
        logger.info('*** directory is empty or all files/dirs excluded ***')

        es.indices.refresh(index)
        res = es.count(index=index, body={'query':{'query_string':{'query':'type:(file OR directory)'}}})['count']
        if res == 0:
            logger.info('*** deleting empty index {0} ***'.format(index))
            es.indices.delete(index=index, ignore=[400, 404])
    else:
        es.index(index, sizes[root])
        total_documents[root] += 1

        elastic.index_end(es, index, root, sizes[root]['size'], 
            sizes[root]['size_du'], filecount[root], dircount[root], 
            end_time, scandir_walk_time)

        logger.info('Walked ({0})'.format(root))
        logger.info('Files Walked {0}, Skipped {1}'.format(filecount[root], skipfilecount[root]))
        logger.info('Walked File Size not factoring in holes {0}'.format(cliutil.convert_size(sizes[root]['size'])))
        logger.info('Disk Usage (du) of walked directory {0}'.format(cliutil.convert_size(sizes[root]['size_du'])))
        logger.info('Directories Walked {0}, Skipped {1}'.format(dircount[root], skipdircount[root]))
        logger.info('Crawl Took {0}'.format(cliutil.get_time(scandir_walk_time)))
        logger.info('Crawl Performance {0:.2f} inodes/s'.format(inodecount[root] / scandir_walk_time))
        logger.info('Elastic Search Documents Count {0}'.format(total_documents[root]))
        logger.info('Elastic Search Index Performance {0:.2f} documents/s'.format(total_documents[root] / scandir_walk_time))
        logger.info('Upload time: {0}'.format(cliutil.get_time(bulktime[root])))
        logger.info('Warnings Count: {0}'.format(warnings))


@click.group(help="Crawl Scoutfs")
@cliutil.pass_context
def sfcli(ctx, **kwargs):
    pass


@sfcli.command(help="Scan scoutFS")
@click.option("--index", default="scoutfs-<dir_tree>-<datetime>", help="optional index name, requires prefix of scoutfs-")
@click.option("--path", required=True, type=click.Path(exists=True), help="path to scan")
@click.option("--maxdepth", default=999, help="max depth to crawl")
@cliutil.pass_context
def scan(ctx, index, path, maxdepth):
    es = elastic.connection()
    print("ES: {}".format(es))

    # check index name to make sure it starts with scoutfs-
    if index and not 'scoutfs-' in index:
        logger.error('Index name prefix scoutfs- required!')
        sys.exit(1)

    #strip trailing slash
    dir_tree = path
    if dir_tree != '/':
        dir_tree = path.rstrip('/')
    dir_tree = os.path.abspath(dir_tree)

    # check if path is empty
    count = 0
    for content in os.scandir(dir_tree):
        if content.is_symlink():
            pass
        elif content.is_dir() or content.is_file():
            count += 1
    if count == 0:
        logger.info('{0} is empty, nothing to crawl.'.format(dir_tree))
        sys.exit(0)

    # check if no index supplied with -i and set default index name
    if index == 'scoutfs-<dir_tree>-<datetime>':
        tree_dir_str = dir_tree
        tree_dir_str = tree_dir_str.replace(' ', '_')
        # replace any forward slash with underscore
        tree_dir_str = tree_dir_str.replace('/', '_')
        index = 'scoutfs' + tree_dir_str.lower().lstrip('_') + '-' + datetime.now().strftime("%y%m%d%H%M%S")

    # check if index exists
    indexexist = elastic.check_index(index, es)
    if indexexist:
        logger.warning('Index {0} already exists'.format(index))
        sys.exit(1)

    # print config being used
    config_filename = os.path.join(config.config_dir(), confuse.CONFIG_FILENAME)
    logger.info('Config file: {0}'.format(config_filename))

    try:
        logger.info('Creating Elastic Search index {0}'.format(index))
        elastic.create_index(index, es)
        elastic.tune_index(es, index)
        
        dircount[dir_tree] = 1
        filecount[dir_tree] = 0
        bulktime[dir_tree] = 0.0
        inodecount[dir_tree] = 0
        skipdircount[dir_tree] = 0
        skipfilecount[dir_tree] = 0
        total_documents[dir_tree] = 0

        start_time = datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%S")
        elastic.index_start(es, index, dir_tree, start_time)

        crawl(dir_tree, es, index, maxdepth)
        
        exit_app(es, index)

    except KeyboardInterrupt:
        logger.info('*** Keyboard interrupt ***')
        exit_app(es, index)
    except Exception as e:                    
        logger.critical('FATAL ERROR: exception: {0}'.format(e), exc_info=True)
        exit_app_critical_error()

@sfcli.command(help="Crawler Version")
@cliutil.pass_context
def version(ctx, **kwargs):
    version = '0.1'
    __version__ = version
    print('v{}'.format(version))

