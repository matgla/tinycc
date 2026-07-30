struct display {
  struct disphist *hstent;
  int pid;
  int status;
};

struct disphist {
  struct disphist *next;
  char *name;
  int startTries;
  unsigned rLogin : 2;
  unsigned sd_how : 2;
  unsigned sd_when : 2;
  unsigned lock : 1;
  unsigned goodExit : 1;
  char *nuser;
  char *npass;
  char **nargs;
};

__attribute__((noinline))
void clear_display(struct display *d)
{
  d->pid = 0;
  d->status = 0;
  d->hstent->lock = d->hstent->rLogin = d->hstent->goodExit =
      d->hstent->sd_how = d->hstent->sd_when = 0;
}
