#!/bin/sh
REVISION="699cb9a11b1dce1515a4f591274b90269962cfec"
echo ""
echo "Branch differs by Commits: #" `git log --oneline $REVISION.. | wc -l`
echo ""
git diff $REVISION
