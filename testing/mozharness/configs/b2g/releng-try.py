#!/usr/bin/env python
import os
config = {
    "default_actions": [
        'clobber',
        'checkout-sources',
        'get-blobs',
        'update-source-manifest',
        'build',
        'build-symbols',
        'prep-upload',
        'upload',
    ],
    "upload": {
        "default": {
            "ssh_key": os.path.expanduser("~/.ssh/b2gtry_dsa"),
            "ssh_user": "b2gtry",
            "upload_remote_host": "pvtbuilds2.dmz.scl3.mozilla.com",
            "upload_remote_path": "/pub/mozilla.org/b2g/try-builds/%(user)s-%(revision)s/%(branch)s-%(target)s",
            "upload_dep_target_exclusions": [],
        },
    },
    "gittool_share_base": "/builds/git-shared/git",
    "gittool_base_mirror_urls": [],
    "vcs_share_base": "/builds/hg-shared",
    "sendchange_masters": ["buildbot-master81.build.mozilla.org:9301"],
    "exes": {
        "tooltool.py": "/tools/tooltool.py",
        "buildbot": "/tools/buildbot/bin/buildbot",
    },
    "env": {
        "CCACHE_DIR": "/builds/ccache",
        "CCACHE_COMPRESS": "1",
        "CCACHE_UMASK": "002",
        "GAIA_OPTIMIZE": "1",
        "WGET_OPTS": "-c -q",
        "PATH": "/tools/python27/bin:%(PATH)s",
    },
    #"clobberer_url": "https://api-pub-build.allizom.org/clobberer/lastclobber",
    #"clobberer_url": "https://api.pub.build.mozilla.org/clobberer/lastclobber",
    "is_automation": True,
    "force_clobber": True,
    "repo_mirror_dir": "/builds/git-shared/repo",
    "repo_remote_mappings": {
        'https://android.googlesource.com/': 'https://git.mozilla.org/external/aosp',
        'git://codeaurora.org/': 'https://git.mozilla.org/external/caf',
        'https://git.mozilla.org/b2g': 'https://git.mozilla.org/b2g',
        'git://github.com/mozilla-b2g/': 'https://git.mozilla.org/b2g',
        'git://github.com/mozilla/': 'https://git.mozilla.org/b2g',
        'https://git.mozilla.org/releases': 'https://git.mozilla.org/releases',
        'http://android.git.linaro.org/git-ro/': 'https://git.mozilla.org/external/linaro',
        'git://github.com/apitrace/': 'https://git.mozilla.org/external/apitrace',
    },
}
