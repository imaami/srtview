# Coding guidelines

## Repository

- Always sort `.gitignore` in the C locale.
  Run this pre-commit but after `git add`:
  ```bash
  x=$(LC_ALL=C sort .gitignore) && {
    sha256sum <<< "$x" | grep -Fqx \
    "$(sha256sum < .gitignore)" || {
      echo "$x" > .gitignore;
      x=$(git diff -- .gitignore) &&
      {
        echo '+============================+';
        echo '[ .gitignore has been sorted ]';
        echo '+============================+';
        echo "$x";
        echo '+============================+';
        echo '[ the working tree needs you ]';
        echo '+============================+';
      } >&2;
    };
  };
  ```
