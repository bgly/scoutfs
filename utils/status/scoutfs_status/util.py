#
# Copyright (c) 2021 Versity Software, Inc. All rights reserved.
#

import os
import re
import click
import core
import math


class Context():
    def __init__(self):
        self.json = False
        self.summary = False
        self.detail = False


pass_context = click.make_pass_decorator(Context, ensure=True)


def display_output(outval, ctx, typename):
    if isinstance(outval, list):
        # For lists, default view is summary
        if ctx.json:
            output = {}
            output['total'] = len(outval)
            output[typename] = outval
            click.echo(core.to_json(output))
        elif ctx.detail:
            click.echo("TOTAL: %s" % str(len(outval)))
            for elem in outval:
                click.echo(core.to_detail_string(elem))
        else:
            click.echo(core.to_table_string(outval))
    else:
        # For single objects, default view is detail
        if ctx.json:
            click.echo(core.to_json(outval))
        elif ctx.summary:
            click.echo(core.to_table_string(outval))
        else:
            click.echo(core.to_detail_string(outval))


def display_string(value):
    if hasattr(value, "_show_dict"):
        return re.sub("'", "", str(value.__dict__))
    elif value is None:
        return "N/A"
    return str(value)


def get_time(seconds):
    m, s = divmod(seconds, 60)
    h, m = divmod(m, 60)
    d, h = divmod(h, 24)
    return "%dd:%dh:%02dm:%02ds" % (d, h, m, s)


def set_times(path, atime, mtime):
    try:
        os.utime(path, (atime, mtime))
    except OSError as e:
        return False, e
    return True, None


def convert_size(size_bytes):
    if size_bytes == 0:
        return '0 B'
    size_name = ('B', 'KB', 'MB', 'GB', 'TB', 'PB', 'EB', 'ZB', 'YB')
    i = int(math.floor(math.log(size_bytes, 1024)))
    p = math.pow(1024, i)
    s = round(size_bytes / p, 2)
    return '{0} {1}'.format(s, size_name[i])


def handle_unicode(f, ignore_errors=False):
    if ignore_errors:
        err = 'replace'
    else:
        err = 'strict'
    try:
        return f.encode('utf-8', errors=err).decode('utf-8')
    except UnicodeEncodeError:
        raise UnicodeError
        