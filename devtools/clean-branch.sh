#!/bin/sh

# A POSIX variable
OPTIND=1         # Reset in case getopts has been used previously in the shell.

help() {

cat >&2 << helpMessage

  Usage: ${0##*/} [ -b <branch-name> -s <suffix> -r <remote-name> -p]

    ${0##*/} will remove DPDK proprietery code from original branch and
    merge the changes to already existing clean branch <branch-name>-<suffix>.
    If -p option is specified the changes will be pushed to the git
    (according to specified remote name)

  Options:

    -h/?        show help (this file)
    -b <name>   original branch name. Optional parameter, if not specified,
                the current branch name will be used
    -s <suffix> suffix for cleaned branch name, if not specified, 'public'
                suffix will be used
    -r <name>   git remote name for fetching clean branch.
                Optional parameter, if not specified, "origin" name will be used
    -p          push new branch to git

helpMessage

  exit ${ecode}
}

# Delete all created branches
clean()
{
    git checkout ${cur_branch}
    git rev-parse --verify --quiet ${branch}-tmp
    if [ $? -eq 0 ] ; then
        git branch -D ${branch}-tmp
    fi
    git rev-parse --verify --quiet ${branch}-${suffix}
    if [ $? -eq 0 ] ; then
        git branch -D ${branch}-${suffix}
    fi
}

# Initialize our own variables:
cur_branch=$(git rev-parse --abbrev-ref HEAD)
branch=${cur_branch}
remote="origin"
suffix="public"
push=0


while getopts "h?pb:s:r:" opt; do
    case "$opt" in
    h|\?)
        help
        exit 0
        ;;
    b)  branch=$OPTARG
        ;;
    s)  suffix=$OPTARG
        ;;
    r)  remote=$OPTARG
        ;;
    p)  push=1
        ;;
    esac
done

shift $((OPTIND-1))

[ "${1:-}" = "--" ] && shift

echo "branch=$branch, suffix=$suffix, remote=$remote, push=$push, Leftovers: $@"

# Create temporary branch "branch-name"-tmp (delete old if exist before)
clean
git branch ${branch}-tmp

# Checkout "branch-name"-tmp
git checkout ${branch}-tmp

if [ $? -ne 0 ] ; then
    echo "git checkout $branch-tmp failed"
    exit 1
fi

# Run filter-branch to remove lport and vport files
time git filter-branch --force --index-filter '\
git rm -rf --cached --ignore-unmatch drivers/net/lport \
git rm -rf --cached --ignore-unmatch drivers/net/vport \
' --prune-empty @ b885c7a..HEAD
if [ $? -ne 0 ] ; then
    echo "git filter-branch failed"
    clean
    exit 1
fi

# Checkout "remote-name"/"branch-name"-"suffix"
git fetch ${remote}
git checkout ${remote}/${branch}-${suffix} -b ${branch}-${suffix}
if [ $? -ne 0 ] ; then
    echo "git checkout $remote/$branch-$suffix failed"
    clean
    exit 1
fi

# Merge new changed to "branch-name"-"suffix"
git merge ${branch}-tmp
if [ $? -ne 0 ] ; then
    echo "git merge $branch-tmp failed"
    git status
    git reset --hard HEAD
    clean
    exit 1
fi

if [ ${push} -eq 1 ] ; then
    url=$(git remote get-url --push $remote)
    echo "Push to url=$url"
    # Push changed to "branch-name"-"suffix"
    git push ${url} HEAD:${branch}-${suffix}
    if [ $? -ne 0 ] ; then
        echo "git push $url HEAD:$branch-$suffix failed"
    fi
    clean
fi

git checkout ${cur_branch}
