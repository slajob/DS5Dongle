#!/bin/sh
# Entry point of the Decky CLI backend container: the backend directory is
# mounted at /backend and everything in /backend/out ends up in the plugin's bin/.
set -e
cd /backend
make
