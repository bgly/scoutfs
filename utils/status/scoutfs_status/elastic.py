#!/usr/bin/env python3
#
# Copyright (c) 2021 Versity Software, Inc. All rights reserved.
#

import os
import sys
import confuse
import logging
import requests
import elasticsearch

from elasticsearch import helpers
from elasticsearch.helpers.errors import BulkIndexError

logger = logging.getLogger(__name__)

"""Load yaml config file."""
config = confuse.Configuration('scoutfs', __name__)

# check for any env vars to override config
try:
    elastic_host = os.getenv('ELASTIC_HOST', config['databases']['elasticsearch']['host'].get())
    elastic_port = os.getenv('ELASTIC_PORT', config['databases']['elasticsearch']['port'].get())
    elastic_username = os.getenv('ELASTIC_USERNAME', config['databases']['elasticsearch']['user'].get())
    if not elastic_username:
        elastic_username = ""
    elastic_pass = os.getenv('ELASTIC_PASS', config['databases']['elasticsearch']['password'].get())
    if not elastic_pass:
        elastic_pass = ""
    elastic_https = os.getenv('ELASTIC_HTTPS', config['databases']['elasticsearch']['https'].get())
    elastic_http_compress = config['databases']['elasticsearch']['httpcompress'].get()
    elastic_timeout = config['databases']['elasticsearch']['timeout'].get()
    elastic_maxsize = config['databases']['elasticsearch']['maxsize'].get()
    elastic_max_retry = config['databases']['elasticsearch']['maxretries'].get()
    elastic_status_yellow = config['databases']['elasticsearch']['wait'].get()
    elastic_chunksize = config['databases']['elasticsearch']['chunksize'].get()
    elastic_transaction_logsize = config['databases']['elasticsearch']['translogsize'].get()
    elastic_transaction_logsyncint = config['databases']['elasticsearch']['translogsyncint'].get()
    elastic_indexrefresh = config['databases']['elasticsearch']['indexrefresh'].get()
except confuse.NotFoundError as e:
    print(
        'Config ERROR: {0}, check config for errors'.format(e))
    sys.exit(1)


def bulk_upload(es, indexname, docs):
    if elastic_status_yellow:
        es.cluster.health(wait_for_status='yellow', request_timeout=elastic_timeout)
    try:
        helpers.bulk(es, docs, index=indexname, chunk_size=elastic_chunksize,
                     request_timeout=elastic_timeout)
    except BulkIndexError as e:
        logger.critical( 'ERROR: Elasticsearch bulk upload error ({})'.format(e))
        raise BulkIndexError(e)


def check_index(name, es):
    if es.indices.exists(index=name):
        return True
    return False


def tune_index(es, indexname, defaults=False):
    default = {
        "index": {
            "number_of_replicas": 0,
            "refresh_interval": "1s",
            "translog.sync_interval": "5s",
            "translog.durability": "request",
            "translog.flush_threshold_size": "512mb"
        }
    }
    tuned = {
        "index": {
            "number_of_replicas": 0,
            "translog.durability": "async",
            "refresh_interval": elastic_indexrefresh,
            "translog.sync_interval": elastic_transaction_logsyncint,
            "translog.flush_threshold_size": elastic_transaction_logsize
        }
    }
    if not defaults:
        logger.info("Tuning index settings for crawl")
        es.indices.put_settings(index=indexname, body=tuned,
                                request_timeout=elastic_timeout)
    else:
        logger.info("Setting index settings back to defaults")
        es.indices.put_settings(index=indexname, body=default,
                                request_timeout=elastic_timeout)


def create_index(indexname, es):
    indexexists = check_index(indexname, es)
    if indexexists:
        logger.info('ES index {0} already exists, deleting'.format(indexname))
        es.indices.delete(index=indexname, ignore=[400, 404])

    mappings = {
        'settings': {
            'index': {
                'number_of_shards': 1,
                'number_of_replicas': 0
            },
            'analysis': {
                'tokenizer': {
                    'filename_tokenizer': {
                        'type': 'char_group',
                        'tokenize_on_chars': [
                            'whitespace',
                            'punctuation',
                            '-',
                            '_'
                        ]
                    },
                    'path_tokenizer': {
                        'type': 'char_group',
                        'tokenize_on_chars': [
                            'whitespace',
                            'punctuation',
                            '/',
                            '-',
                            '_'
                        ]
                    }
                },
                'analyzer': {
                    'filename_analyzer': {
                        'tokenizer': 'filename_tokenizer',
                        'filter': [
                            'camel_filter',
                            'lowercase'
                        ]
                    },
                    'path_analyzer': {
                        'tokenizer': 'path_tokenizer',
                        'filter': [
                            'camel_filter',
                            'lowercase'
                        ]
                    }
                },
                'filter': {
                    'camel_filter': {
                        'type': 'word_delimiter_graph',
                        'generate_number_parts': 'false',
                        'stem_english_possessive': 'false',
                        'split_on_numerics': 'false'
                    }
                }
            }
        },
        'mappings': {
            'properties': {
                'name': {
                    'type': 'keyword',
                            'fields': {
                                'text': {
                                    'type': 'text',
                                    'analyzer': 'filename_analyzer'
                                }
                            }
                },
                'parent_path': {
                    'type': 'keyword',
                            'fields': {
                                'text': {
                                    'type': 'text',
                                    'analyzer': 'path_analyzer'
                                }
                            }
                },
                'size': {
                    'type': 'long'
                },
                'size_du': {
                    'type': 'long'
                },
                'file_count': {
                    'type': 'long'
                },
                'dir_count': {
                    'type': 'long'
                },
                'owner': {
                    'type': 'keyword'
                },
                'group': {
                    'type': 'keyword'
                },
                'mtime': {
                    'type': 'date'
                },
                'atime': {
                    'type': 'date'
                },
                'ctime': {
                    'type': 'date'
                },
                'nlink': {
                    'type': 'integer'
                },
                'ino': {
                    'type': 'keyword'
                },
                'extension': {
                    'type': 'keyword'
                },
                'path': {
                    'type': 'keyword'
                },
                'total': {
                    'type': 'long'
                },
                'used': {
                    'type': 'long'
                },
                'free': {
                    'type': 'long'
                },
                'available': {
                    'type': 'long'
                },
                'file_size': {
                    'type': 'long'
                },
                'file_size_du': {
                    'type': 'long'
                },
                'file_count': {
                    'type': 'long'
                },
                'dir_count': {
                    'type': 'long'
                },
                'start_at': {
                    'type': 'date'
                },
                'end_at': {
                    'type': 'date'
                },
                'crawl_time': {
                    'type': 'float'
                },
                'type': {
                    'type': 'keyword'
                }
            }
        }
    }

    try:
        es.indices.create(index=indexname, body=mappings)
    except elasticsearch.ConnectionError as e:
        print('ERROR: Can not connect to Elasticsearch ({})'.format(e))
        sys.exit(1)
    return True


def index_start(es, index, path, start):
    mounts = []
    mounts.append(path)
    for entry in os.scandir(path):
        if entry.is_symlink():
            pass
        elif entry.is_dir():
            if os.path.ismount(entry.path):
                mounts.append(entry.path)
    for mount_path in mounts:
        statvfs = os.statvfs(mount_path)
        total = statvfs.f_frsize * statvfs.f_blocks
        free = statvfs.f_frsize * statvfs.f_bfree
        available = statvfs.f_frsize * statvfs.f_bavail
        data = {
            'path': mount_path,
            'free': free,
            'total': total,
            'used': total - free,
            'available': available,
            'type': 'spaceinfo'
        }
        es.index(index=index, body=data)
    data = {
            'path': path,
            'start_at': start,
            'type': 'indexinfo'
        }
    es.index(index=index, body=data)


def index_end(es, index, path, size, size_du, filecount, dircount, end, elapsed):
    data = {
        'path': path,
        'end_at': end,
        'file_size': size,
        'dir_count': dircount,
        'crawl_time': elapsed,
        'file_size_du': size_du,
        'file_count': filecount,
        'type': 'indexinfo'
    }
    es.index(index=index, body=data)


def connection():
    if elastic_https:
        scheme = 'https'
    else:
        scheme = 'http'
    url = scheme + '://' + elastic_host + ':' + str(elastic_port)
    try:
        r = requests.get(url, auth=(elastic_username, elastic_pass))
    except Exception as e:
        print(
            'Error connecting to Elasticsearch, check config and Elasticsearch is running.\n\nError: {0}'.format(e))
        sys.exit(1)

    # Check if we are using HTTP TLS/SSL
    if elastic_https:
        es = elasticsearch.Elasticsearch(
            hosts=elastic_host,
            port=elastic_port,
            http_auth=(elastic_username, elastic_pass),
            scheme="https", use_ssl=True, verify_certs=True,
            timeout=elastic_timeout, maxsize=elastic_maxsize,
            connection_class=elasticsearch.RequestsHttpConnection,
            max_retries=elastic_max_retry, retry_on_timeout=True, http_compress=elastic_http_compress)
    # Local connection to es
    else:
        es = elasticsearch.Elasticsearch(
            hosts=elastic_host,
            port=elastic_port,
            http_auth=(elastic_username, elastic_pass),
            timeout=elastic_timeout, maxsize=elastic_maxsize,
            connection_class=elasticsearch.Urllib3HttpConnection,
            max_retries=elastic_max_retry, retry_on_timeout=True, http_compress=elastic_http_compress)

    return es
