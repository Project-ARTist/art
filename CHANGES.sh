#!/bin/sh
REVISION="12eb0c532e33ca5d5e98addd580b5ad0a4b71be4"
echo ""
echo "Branch differs by Commits: #" `git log --oneline $REVISION.. | wc -l`
echo ""
git diff $REVISION
