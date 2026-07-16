# Coding guidelines

## Repository

- Keep `.gitignore` sorted in the C locale.

```bash
x=$(LC_ALL=C sort .gitignore) && {
  sha256sum <<< "$x" | grep -Fqx \
  "$(sha256sum < .gitignore)" || {
    echo "$x" > .gitignore ;
    {
      printf '%s\n' \
             'Sorting of .gitignore fixed,' \
             'please git add && git commit' \
             'changes in the working tree:' ;
      git diff -- .gitignore ;
    } >&2 ;
  } ;
} ;
```
