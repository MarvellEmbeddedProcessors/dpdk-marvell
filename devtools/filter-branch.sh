#!/bin/sh

# Run filter-branch to remove lport and vport files
git filter-branch --force --index-filter '\
git rm -rf --cached --ignore-unmatch drivers/net/lport \
git rm -rf --cached --ignore-unmatch drivers/net/vport \
git rm -rf --cached --ignore-unmatch doc/guides/nics/lport.rst \
' --prune-empty @ b885c7a..HEAD

