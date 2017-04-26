#!/bin/sh
REVISION="8d5c5f437726c1b996e2be5c3fac158a8eea2200"
echo ""
echo "Branch differs by Commits: #" `git log --oneline $REVISION.. | wc -l`
echo ""
git diff $REVISION
