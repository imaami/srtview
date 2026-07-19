#!/usr/bin/env bash
# CodeRabbit PR plumbing — deterministic only; judgment lives in SKILL.md
# usage: cr.sh fetch
#        cr.sh reply <thread-id> <body>
#        cr.sh resolve <thread-id> | unresolve <thread-id>
#        cr.sh comment <body>

# gh api --jq is gojq with no --arg plumbing, so the bot name is
# inlined below.  GraphQL reports the login as "coderabbitai", REST
# as "coderabbitai[bot]"; both match with startswith.

fail() { printf 'cr.sh: %s\n' "$*" >&2; exit 1; }

ctx()
{
	repo=$(gh repo view --json owner,name -q '.owner.login+" "+.name' 2>&1) ||
		fail "gh repo view: $repo"
	read -r owner name <<<"$repo"
	pr=$(gh pr view --json number -q .number 2>&1) ||
		fail "no PR for current branch: $pr"
}

fetch()
{
	ctx

	# Buffer all three surfaces; emit nothing unless every fetch succeeded,
	# so a partial set can never masquerade as complete.
	inline=$(gh api graphql --paginate \
		-f owner="$owner" -f repo="$name" -F pr="$pr" -f query='
query($owner:String!,$repo:String!,$pr:Int!,$endCursor:String){
  repository(owner:$owner,name:$repo){ pullRequest(number:$pr){
    reviewThreads(first:100,after:$endCursor){
      pageInfo{ hasNextPage endCursor }
      nodes{ id isResolved isOutdated path line
        comments(first:50){ nodes{ author{login} body url }}}}}}}' \
		--jq '
.data.repository.pullRequest.reviewThreads.nodes[]
| select(.comments.nodes[0].author.login|startswith("coderabbitai"))
| {source:"inline", thread:.id, resolved:.isResolved,
   outdated:.isOutdated, path, line,
   url:.comments.nodes[0].url,
   replies:(.comments.nodes|length-1),
   body:.comments.nodes[0].body}') ||
		fail "inline fetch (graphql)"

	reviews=$(gh api "repos/$owner/$name/pulls/$pr/reviews" --paginate \
		--jq '.[]|select(.user.login|startswith("coderabbitai"))
		      |{source:"review",id,state,body}') ||
		fail "reviews fetch"

	issues=$(gh api "repos/$owner/$name/issues/$pr/comments" --paginate \
		--jq '.[]|select(.user.login|startswith("coderabbitai"))
		      |{source:"issue",id,url:.html_url,body}') ||
		fail "issue comments fetch"

	printf '%s\n%s\n%s\n' "$inline" "$reviews" "$issues"
}

reply()
{
	[[ $1 && $2 ]] || fail "usage: reply <thread-id> <body>"
	gh api graphql -f tid="$1" -f body="$2" -f query='
mutation($tid:ID!,$body:String!){
  addPullRequestReviewThreadReply(
    input:{pullRequestReviewThreadId:$tid,body:$body})
  { comment { url } }}' \
		--jq .data.addPullRequestReviewThreadReply.comment.url ||
		fail "reply to $1"
}

setres()
{
	[[ $2 ]] || fail "usage: $1 <thread-id>"
	gh api graphql -f tid="$2" -f query="
mutation(\$tid:ID!){ ${1}ReviewThread(input:{threadId:\$tid})
{ thread { isResolved }}}" \
		--jq ".data.${1}ReviewThread.thread.isResolved" ||
		fail "$1 $2"
}

case $1 in
fetch)     fetch ;;
reply)     reply "$2" "$3" ;;
resolve)   setres resolve "$2" ;;
unresolve) setres unresolve "$2" ;;
comment)   [[ $2 ]] || fail "usage: comment <body>"
           gh pr comment --body "$2" || fail "pr comment" ;;
*) fail "usage: fetch | reply <tid> <body> | resolve <tid> | unresolve <tid> | comment <body>" ;;
esac
