;;; jl-magit-git-helper.el --- Answer Magit's metadata git calls from libgit2 -*- lexical-binding: t; -*-

;;; Commentary:

;; Starting a git process costs ~100 ms on Windows, and most of the git
;; processes in a Magit status refresh only look up metadata: refs,
;; config, HEAD, a few log lines, the worktree list.  This answers those
;; from jl-git-helper, a small libgit2 program (helper/ in this project)
;; that Emacs starts once and keeps running.
;;
;; It hooks into `magit-process-file', through which Magit runs git
;; synchronously.  The helper reproduces git's stdout byte for byte for a
;; short list of read-only commands and declines everything else, which
;; then runs git as before.  So the worst case is today's speed, and
;; Magit and jl-magit-prefill need no changes.
;;
;; Setup:
;;   1. Build helper/ (see helper/Makefile) and put the executable (on
;;      Windows with the DLLs from "make dist") in the directory named by
;;      `jl/magit-git-helper-program'.
;;   2. Load this file after magit.
;;   3. First on a new machine: (setq jl/magit-git-helper-mode 'verify),
;;      use Magit for a while and check `M-x jl/magit-git-helper-report'.
;;      In verify mode git still runs and its answer is used; the
;;      helper's answer is only compared with it.
;;
;; Requires git >= 2.42 (for `git var GIT_CONFIG_SYSTEM').

;;; Code:

(require 'cl-lib)
(require 'magit)

(defgroup jl-magit-git-helper nil
  "Answer Magit's metadata git calls from a libgit2 helper."
  :group 'magit)

(defcustom jl/magit-git-helper-program
  (expand-file-name (if (eq system-type 'windows-nt)
                        "jl-git-helper/jl-git-helper.exe"
                      "jl-git-helper/jl-git-helper")
                    user-emacs-directory)
  "The jl-git-helper executable."
  :type 'file)

(defcustom jl/magit-git-helper-mode t
  "Whether Magit's git calls are answered by the helper.
nil: never.  t: whenever the helper can.  `verify': run git as well,
use git's answer and record any difference (see
`jl/magit-git-helper-report')."
  :type '(choice (const :tag "Off" nil)
                 (const :tag "On" t)
                 (const :tag "Compare with git" verify)))

(defcustom jl/magit-git-helper-timeout 5
  "Seconds to wait for an answer before giving up on the helper."
  :type 'number)

(defvar jl/magit-git-helper--process nil)
(defvar jl/magit-git-helper--disabled nil
  "Why the helper is not used (a string), e.g. because it failed to start.")
(defvar jl/magit-git-helper--busy nil)

(defun jl/magit-git-helper--fresh-stats ()
  (list :answered 0 :declined 0 :seconds 0.0 :verified 0 :mismatches 0))

(defvar jl/magit-git-helper-stats (jl/magit-git-helper--fresh-stats)
  "Counters since the helper was started; read by jl-magit-trace.")

(defconst jl/magit-git-helper--refused-env
  (concat "\\`GIT_\\(DIR\\|WORK_TREE\\|COMMON_DIR\\|INDEX_FILE\\|OBJECT_DIRECTORY"
          "\\|ALTERNATE_OBJECT_DIRECTORIES\\|NAMESPACE\\|CONFIG"
          "\\|CEILING_DIRECTORIES\\|REPLACE_REF_BASE\\)")
  "Variables that change what git sees; with any of them set, run git.")

;;; Process

(defun jl/magit-git-helper--start ()
  "Start the helper and return its process, or nil."
  (let* ((default-directory (file-name-as-directory
                             (expand-file-name temporary-file-directory)))
         (process-environment (magit-process-environment))
         (program (and (file-executable-p jl/magit-git-helper-program)
                       jl/magit-git-helper-program))
         (sysconfig
          (and program
               (ignore-errors
                 (car (process-lines magit-git-executable "var" "GIT_CONFIG_SYSTEM"))))))
    (cond
      ((not program)
       (setq jl/magit-git-helper--disabled
             (format "%s not found" jl/magit-git-helper-program))
       nil)
      ((not sysconfig)
       (setq jl/magit-git-helper--disabled
             "`git var GIT_CONFIG_SYSTEM' failed (git older than 2.42?)")
       nil)
      (t
       (let ((buf (generate-new-buffer " *jl-git-helper*")))
         (with-current-buffer buf (set-buffer-multibyte nil))
         (setq jl/magit-git-helper-stats (jl/magit-git-helper--fresh-stats))
         (setq jl/magit-git-helper--process
               (make-process
                :name "jl-git-helper"
                :buffer buf
                :command (list jl/magit-git-helper-program
                               (concat "--system-config=" sysconfig))
                :coding 'binary
                :connection-type 'pipe
                :noquery t
                :stderr (get-buffer-create " *jl-git-helper stderr*")
                :sentinel (lambda (proc _event)
                            (unless (process-live-p proc)
                              (when (eq proc jl/magit-git-helper--process)
                                (setq jl/magit-git-helper--process nil))
                              (kill-buffer (process-buffer proc)))))))))))

(defun jl/magit-git-helper--live-process ()
  (cond ((process-live-p jl/magit-git-helper--process) jl/magit-git-helper--process)
        (jl/magit-git-helper--disabled nil)
        (t (jl/magit-git-helper--start))))

(defun jl/magit-git-helper-stop ()
  "Stop the helper.  It starts again on the next git call."
  (interactive)
  (when (process-live-p jl/magit-git-helper--process)
    (delete-process jl/magit-git-helper--process))
  (setq jl/magit-git-helper--process nil
        jl/magit-git-helper--disabled nil))

(defun jl/magit-git-helper--parse (buf)
  "Return (EXIT . STDOUT) once BUF holds a complete answer."
  (with-current-buffer buf
    (goto-char (point-min))
    (when (looking-at "\\(-?[0-9]+\\) \\([0-9]+\\)\n")
      (let ((code (string-to-number (match-string 1)))
            (len (string-to-number (match-string 2)))
            (start (match-end 0)))
        (when (>= (- (point-max) start) len)
          (cons code (buffer-substring-no-properties start (+ start len))))))))

(defun jl/magit-git-helper--request (dir args)
  "Ask the helper about git ARGS in DIR.
Return (EXIT . STDOUT), with STDOUT undecoded, or nil if it failed.
EXIT -1 means declined; STDOUT then says why."
  (when-let* ((proc (jl/magit-git-helper--live-process)))
    (let ((buf (process-buffer proc))
          (deadline (+ (float-time) jl/magit-git-helper-timeout))
          result)
      (with-current-buffer buf
        (erase-buffer)
        (set-marker (process-mark proc) (point-min)))
      (process-send-string
       proc (concat (encode-coding-string (string-join (cons dir args) "\t") 'utf-8)
                    "\n"))
      (while (and (not result)
                  (process-live-p proc)
                  (< (float-time) deadline))
        ;; Integer JUST-THIS-ONE: no other process output, no timers.
        (accept-process-output proc 0.05 nil 1)
        (setq result (jl/magit-git-helper--parse buf)))
      (unless result
        (jl/magit-git-helper-stop)
        (setq jl/magit-git-helper--disabled "no answer from the helper")
        (message "jl-git-helper: no answer; using git (M-x jl/magit-git-helper-stop resets)"))
      result)))

;;; Serving magit-process-file

(defun jl/magit-git-helper--target (destination)
  "Split `call-process' DESTINATION into (BUFFER-OR-NIL . STDERR).
STDERR is `discard', `file' or `mixed'.  Return nil if unusable."
  (pcase destination
    ((or 'nil 0) (cons nil 'discard))
    (`(:file . ,_) nil)
    (`(,real ,stderr)
     (let ((buf (pcase real
                  ('t (current-buffer))
                  ((or 'nil 0) nil)
                  (_ (get-buffer real)))))
       (and (or buf (memq real '(nil 0)))
            (cons buf (cond ((null stderr) 'discard)
                            ((eq stderr t) 'mixed)
                            (t 'file))))))
    (`(,real) (jl/magit-git-helper--target (list real nil)))
    ;; t, a buffer or a buffer name: git's stderr would be mixed in.
    (_ (let ((buf (if (eq destination t) (current-buffer) (get-buffer destination))))
         (and buf (cons buf 'mixed))))))

(defun jl/magit-git-helper--usable-p (process infile args)
  (and jl/magit-git-helper-mode
       (not jl/magit-git-helper--busy)
       (null infile)
       (equal process (magit-git-executable))
       (not (file-remote-p default-directory))
       (not (string-match-p "[\t\n\r]" default-directory))
       (cl-every (lambda (a)
                   (and (stringp a)
                        (not (string-match-p "[\t\n\r]\\|[^[:ascii:]]" a))))
                 args)
       (not (seq-some (lambda (v) (string-match-p jl/magit-git-helper--refused-env v))
                      (magit-process-environment)))))

(defun jl/magit-git-helper--decode (bytes)
  (decode-coding-string
   bytes (or coding-system-for-read (car (magit--process-coding-system)) 'utf-8-unix)
   t))

(defun jl/magit-git-helper--log (dir args fmt &rest objects)
  (with-current-buffer (get-buffer-create "*jl-git-helper log*")
    (goto-char (point-max))
    (insert (format-time-string "%T ") (abbreviate-file-name dir) " git "
            (string-join (jl/magit-git-helper--local-args args) " ") "\n  "
            (apply #'format fmt objects) "\n")))

(defun jl/magit-git-helper--local-args (args)
  "ARGS without the global arguments, for display."
  (cadr (magit-process-git-arguments--split (magit-git-executable) args)))

(defun jl/magit-git-helper--around (orig process &optional infile destination display
                                         &rest args)
  "Around `magit-process-file': answer from the helper when it can."
  (or (and (jl/magit-git-helper--usable-p process infile args)
           (jl/magit-git-helper--try orig process destination display args))
      (apply orig process infile destination display args)))

(defun jl/magit-git-helper--try (orig process destination display args)
  "Answer git ARGS from the helper; return the exit code, or nil."
  (when-let* ((target (jl/magit-git-helper--target destination))
              ((not (eq (cdr target) 'mixed))))
    (let* ((dir (expand-file-name default-directory))
           (t0 (float-time))
           (answer (let ((jl/magit-git-helper--busy t))
                     (jl/magit-git-helper--request dir args)))
           (code (car answer)))
      (cl-incf (plist-get jl/magit-git-helper-stats :seconds) (- (float-time) t0))
      (cond
        ((or (null answer) (< code 0)
             ;; Git would put an error message where the caller reads it.
             (and (/= code 0) (eq (cdr target) 'file)))
         (cl-incf (plist-get jl/magit-git-helper-stats :declined))
         nil)
        ((eq jl/magit-git-helper-mode 'verify)
         (jl/magit-git-helper--verify orig process destination display args
                                      dir target code (cdr answer)))
        (t
         (cl-incf (plist-get jl/magit-git-helper-stats :answered))
         (when (car target)
           (with-current-buffer (car target)
             (insert (jl/magit-git-helper--decode (cdr answer)))))
         code)))))

(defun jl/magit-git-helper--verify (orig process destination display args
                                         dir target code bytes)
  "Run git as well, compare, and deliver git's answer."
  (let* ((stderr (if (consp destination) (cadr destination) nil))
         (text (jl/magit-git-helper--decode bytes))
         git-code git-text)
    (with-temp-buffer
      (setq git-code (apply orig process nil (list t stderr) display args))
      (setq git-text (buffer-string)))
    (cl-incf (plist-get jl/magit-git-helper-stats :verified))
    (unless (and (eql code git-code) (equal text git-text))
      (cl-incf (plist-get jl/magit-git-helper-stats :mismatches))
      (jl/magit-git-helper--log
       dir args "MISMATCH: helper exit %s, git exit %s\n  helper: %S\n  git:    %S"
       code git-code
       (truncate-string-to-width text 2000) (truncate-string-to-width git-text 2000)))
    (when (car target)
      (with-current-buffer (car target) (insert git-text)))
    git-code))

(defun jl/magit-git-helper-report ()
  "Show what the helper did since it was started."
  (interactive)
  (message "jl-git-helper: %s; %d answered, %d declined (%.0f ms in helper); %s"
           (cond (jl/magit-git-helper--disabled
                  (concat "not used: " jl/magit-git-helper--disabled))
                 ((process-live-p jl/magit-git-helper--process) "running")
                 (t "not started"))
           (plist-get jl/magit-git-helper-stats :answered)
           (plist-get jl/magit-git-helper-stats :declined)
           (* 1000 (plist-get jl/magit-git-helper-stats :seconds))
           (if (eq jl/magit-git-helper-mode 'verify)
               (format "verify mode: %d compared, %d mismatches (see *jl-git-helper log*)"
                       (plist-get jl/magit-git-helper-stats :verified)
                       (plist-get jl/magit-git-helper-stats :mismatches))
             (format "mode %s" jl/magit-git-helper-mode))))

(advice-add 'magit-process-file :around #'jl/magit-git-helper--around)

(provide 'jl-magit-git-helper)
;;; jl-magit-git-helper.el ends here
