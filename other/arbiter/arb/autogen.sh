#! /bin/sh

autoreconf=${AUTORECONF:-autoreconf}

$autoreconf ${autoreconf_args:-"-vif"} || exit 1

# Remove the autom4te.cache folders for a release-like structure.
find . -name autom4te.cache | xargs rm -rf
