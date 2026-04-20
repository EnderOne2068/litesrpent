;;; core.lisp -- Litesrpent standard library
;;; Loaded automatically if present next to the executable.
;;; Provides a comprehensive Common Lisp environment.

;;;============================================================
;;; SECTION 1: CORE UTILITY MACROS
;;;============================================================

;; ---- with-gensyms / once-only (needed by later macros) ----

(defmacro with-gensyms (names &body body)
  "Bind each name in NAMES to a fresh gensym."
  `(let ,(mapcar (lambda (n) (list n '(gensym))) names)
     ,@body))

(defmacro once-only (names &body body)
  "Ensure each NAME is evaluated only once in the macro expansion."
  (let ((gensyms (mapcar (lambda (n) (gensym)) names)))
    `(let ,(mapcar #'list gensyms names)
       `(let (,,@(mapcar (lambda (g n) ``(,,g ,,n)) gensyms names))
          ,,@body))))

;;;============================================================
;;; SECTION 2: CONTROL FLOW MACROS
;;;============================================================

;; when / unless are special forms in lseval.c -- skip

;; prog1 / prog2 already defined? Redefine cleanly.
(defmacro prog1 (first &body rest)
  (let ((g (gensym)))
    `(let ((,g ,first)) ,@rest ,g)))

(defmacro prog2 (first second &body rest)
  `(progn ,first (prog1 ,second ,@rest)))

;; return -- expands to return-from nil
(defmacro return (&optional val)
  `(return-from nil ,val))

;; ---- case / ecase ----

(defmacro case (keyform &body clauses)
  "Dispatch on the value of KEYFORM using EQL comparison."
  (let ((g (gensym)))
    `(let ((,g ,keyform))
       (cond
         ,@(mapcar
            (lambda (clause)
              (let ((keys (car clause))
                    (body (cdr clause)))
                (cond
                  ((or (eq keys 't) (eq keys 'otherwise))
                   `(t ,@body))
                  ((listp keys)
                   `((or ,@(mapcar (lambda (k) `(eql ,g ',k)) keys))
                     ,@body))
                  (t
                   `((eql ,g ',keys) ,@body)))))
            clauses)))))

(defmacro ecase (keyform &body clauses)
  "Like CASE but signals an error if no clause matches."
  (let ((g (gensym)))
    `(let ((,g ,keyform))
       (case ,g
         ,@clauses
         (otherwise (error (format nil "ECASE: ~A fell through" ,g)))))))

;; ---- typecase / etypecase ----

(defmacro typecase (keyform &body clauses)
  "Dispatch on the type of KEYFORM."
  (let ((g (gensym)))
    `(let ((,g ,keyform))
       (cond
         ,@(mapcar
            (lambda (clause)
              (let ((type (car clause))
                    (body (cdr clause)))
                (if (or (eq type 't) (eq type 'otherwise))
                    `(t ,@body)
                    `((typep ,g ',type) ,@body))))
            clauses)))))

(defmacro etypecase (keyform &body clauses)
  "Like TYPECASE but signals an error if no clause matches."
  (let ((g (gensym)))
    `(let ((,g ,keyform))
       (typecase ,g
         ,@clauses
         (otherwise (error (format nil "ETYPECASE: ~A fell through" ,g)))))))

;; ---- do / do* ----

(defmacro do (varlist endlist &body body)
  "General iteration macro."
  (let ((vars (mapcar #'car varlist))
        (inits (mapcar (lambda (v) (if (cdr v) (cadr v) nil)) varlist))
        (steps (mapcar (lambda (v) (if (cddr v) (caddr v) nil)) varlist))
        (end-test (car endlist))
        (result-forms (cdr endlist))
        (tag (gensym)))
    `(block nil
       (let ,(mapcar #'list vars inits)
         (tagbody
          ,tag
          (when ,end-test (return (progn ,@result-forms)))
          ,@body
          ,@(let ((step-forms nil))
              (dolist (v varlist (reverse step-forms))
                (when (cddr v)
                  (push `(setq ,(car v) ,(caddr v)) step-forms))))
          (go ,tag))))))

(defmacro do* (varlist endlist &body body)
  "Like DO but with sequential binding."
  (let ((vars (mapcar #'car varlist))
        (inits (mapcar (lambda (v) (if (cdr v) (cadr v) nil)) varlist))
        (end-test (car endlist))
        (result-forms (cdr endlist))
        (tag (gensym)))
    `(block nil
       (let* ,(mapcar #'list vars inits)
         (tagbody
          ,tag
          (when ,end-test (return (progn ,@result-forms)))
          ,@body
          ,@(let ((step-forms nil))
              (dolist (v varlist (reverse step-forms))
                (when (cddr v)
                  (push `(setq ,(car v) ,(caddr v)) step-forms))))
          (go ,tag))))))

;; ---- dotimes / dolist (already exist but redefine cleanly) ----

(defmacro dotimes (spec &body body)
  (let ((var (car spec))
        (count (cadr spec))
        (result (if (cddr spec) (caddr spec) nil))
        (g (gensym)))
    `(let ((,g ,count) (,var 0))
       (block nil
         (tagbody
          loop
          (when (>= ,var ,g) (return ,result))
          ,@body
          (setq ,var (1+ ,var))
          (go loop))))))

(defmacro dolist (spec &body body)
  (let ((var (car spec))
        (list-form (cadr spec))
        (result (if (cddr spec) (caddr spec) nil))
        (g (gensym)))
    `(let ((,g ,list-form) (,var nil))
       (block nil
         (tagbody
          loop
          (when (null ,g) (return ,result))
          (setq ,var (car ,g))
          (setq ,g (cdr ,g))
          ,@body
          (go loop))))))

;; ---- prog / prog* ----

(defmacro prog (varlist &body body)
  `(block nil
     (let ,varlist
       (tagbody ,@body))))

(defmacro prog* (varlist &body body)
  `(block nil
     (let* ,varlist
       (tagbody ,@body))))

;;;============================================================
;;; SECTION 3: SETF INFRASTRUCTURE
;;;============================================================

;; We implement setf as a macro that dispatches on the place form.
;; Since setq handles simple variables, setf handles generalized places.

(defvar *setf-expanders* nil
  "Alist of (accessor-name . expander-fn) for setf.")

(defun %register-setf-expander (name fn)
  (let ((existing (assoc name *setf-expanders*)))
    (if existing
        (rplacd existing fn)
        (setq *setf-expanders* (cons (cons name fn) *setf-expanders*)))))

(defmacro defsetf (accessor lambda-list &body store-body)
  "Simple short-form defsetf: (defsetf accessor (args...) (store-var) body)."
  ;; We store a function that takes (place-args new-value) and returns an expansion
  (let ((store-var (car (car store-body)))
        (body (cdr store-body)))
    `(%register-setf-expander ',accessor
       (lambda (place-args new-val)
         (let ,(mapcar #'list lambda-list 'place-args)
           (let ((,store-var new-val))
             ,@body))))))

(defmacro setf (&rest pairs)
  "Set generalized places."
  (if (null pairs)
      nil
      (if (null (cdr pairs))
          (error "Odd number of args to SETF")
          (let ((place (car pairs))
                (value (cadr pairs))
                (rest (cddr pairs)))
            (if rest
                `(progn (setf ,place ,value)
                        (setf ,@rest))
                (if (symbolp place)
                    `(setq ,place ,value)
                    (let ((accessor (car place))
                          (args (cdr place)))
                      (cond
                        ((eq accessor 'car)
                         (let ((g (gensym)))
                           `(let ((,g ,value))
                              (rplaca ,(car args) ,g) ,g)))
                        ((eq accessor 'cdr)
                         (let ((g (gensym)))
                           `(let ((,g ,value))
                              (rplacd ,(car args) ,g) ,g)))
                        ((eq accessor 'first)
                         (let ((g (gensym)))
                           `(let ((,g ,value))
                              (rplaca ,(car args) ,g) ,g)))
                        ((eq accessor 'rest)
                         (let ((g (gensym)))
                           `(let ((,g ,value))
                              (rplacd ,(car args) ,g) ,g)))
                        ((eq accessor 'nth)
                         (let ((g (gensym)) (g2 (gensym)))
                           `(let ((,g ,(car args))
                                  (,g2 ,value))
                              (rplaca (nthcdr ,g ,(cadr args)) ,g2)
                              ,g2)))
                        ((eq accessor 'aref)
                         `(setf-aref ,(car args) ,(cadr args) ,value))
                        ((eq accessor 'svref)
                         `(setf-aref ,(car args) ,(cadr args) ,value))
                        ((eq accessor 'gethash)
                         `(puthash ,(car args) ,(cadr args) ,value))
                        ((eq accessor 'slot-value)
                         `(set-slot-value ,(car args) ,(cadr args) ,value))
                        ((eq accessor 'symbol-value)
                         (let ((g (gensym)))
                           `(let ((,g ,value))
                              (setq ,(car args) ,g) ;; simplified
                              ,g)))
                        ((eq accessor 'symbol-function)
                         (error "setf of symbol-function not supported"))
                        ((eq accessor 'cadr)
                         (let ((g (gensym)))
                           `(let ((,g ,value))
                              (rplaca (cdr ,(car args)) ,g) ,g)))
                        ((eq accessor 'cdar)
                         (let ((g (gensym)))
                           `(let ((,g ,value))
                              (rplacd (car ,(car args)) ,g) ,g)))
                        ((eq accessor 'caar)
                         (let ((g (gensym)))
                           `(let ((,g ,value))
                              (rplaca (car ,(car args)) ,g) ,g)))
                        ((eq accessor 'cddr)
                         (let ((g (gensym)))
                           `(let ((,g ,value))
                              (rplacd (cdr ,(car args)) ,g) ,g)))
                        ((eq accessor 'caddr)
                         (let ((g (gensym)))
                           `(let ((,g ,value))
                              (rplaca (cddr ,(car args)) ,g) ,g)))
                        (t
                         ;; check dynamic expanders
                         (let ((expander (assoc accessor *setf-expanders*)))
                           (if expander
                               (funcall (cdr expander) args value)
                               ;; Last resort: try (setf-ACCESSOR ...)
                               (let ((setter (intern (concatenate "SETF-"
                                                      (symbol-name accessor)))))
                                 `(,setter ,@args ,value)))))))))))))

;; ---- incf / decf in terms of setf ----

(defmacro incf (place &optional (delta 1))
  "Increment a generalized place."
  (if (symbolp place)
      `(setq ,place (+ ,place ,delta))
      `(setf ,place (+ ,place ,delta))))

(defmacro decf (place &optional (delta 1))
  "Decrement a generalized place."
  (if (symbolp place)
      `(setq ,place (- ,place ,delta))
      `(setf ,place (- ,place ,delta))))

;; ---- push / pop in terms of setf ----

(defmacro push (item place)
  "Push ITEM onto list at PLACE."
  (if (symbolp place)
      `(setq ,place (cons ,item ,place))
      `(setf ,place (cons ,item ,place))))

(defmacro pop (place)
  "Pop the first element from list at PLACE."
  (let ((g (gensym)))
    (if (symbolp place)
        `(let ((,g (car ,place)))
           (setq ,place (cdr ,place))
           ,g)
        `(let ((,g (car ,place)))
           (setf ,place (cdr ,place))
           ,g))))

;; ---- pushnew ----

(defmacro pushnew (item place &key (test '#'eql))
  "Push ITEM onto PLACE if not already a member."
  (let ((g (gensym)))
    `(let ((,g ,item))
       (unless (member ,g ,place)
         (push ,g ,place))
       ,place)))

;; ---- rotatef / shiftf ----

(defmacro rotatef (&rest places)
  "Rotate values of places left: place1 <- place2 <- ... <- placeN <- place1."
  (if (null places)
      nil
      (if (null (cdr places))
          nil
          (let ((temps (mapcar (lambda (p) (gensym)) places)))
            `(let ,(mapcar #'list temps places)
               ,@(let ((sets nil)
                       (n (length places)))
                   (dotimes (i n (reverse sets))
                     (push `(setf ,(nth i places)
                                  ,(nth (mod (1+ i) n) temps))
                           sets)))
               nil)))))

(defmacro shiftf (&rest places-and-newval)
  "Shift values left and return the first evicted value."
  (let* ((places (reverse (cdr (reverse places-and-newval))))
         (newval (car (last places-and-newval)))
         (temps (mapcar (lambda (p) (gensym)) places))
         (result (gensym)))
    `(let ,(mapcar #'list temps places)
       (let ((,result ,(car temps)))
         ,@(let ((sets nil)
                 (n (length places)))
             (dotimes (i (1- n))
               (push `(setf ,(nth i places) ,(nth (1+ i) temps)) sets))
             (push `(setf ,(nth (1- n) places) ,newval) sets)
             (reverse sets))
         ,result))))

;;;============================================================
;;; SECTION 4: LIST ACCESSORS (first through tenth, rest)
;;;============================================================

;; first, rest, second, third are builtins. Define the rest:

(defun fourth  (list) (car (cdddr list)))
(defun fifth   (list) (car (cddddr list)))
(defun sixth   (list) (nth 5 list))
(defun seventh (list) (nth 6 list))
(defun eighth  (list) (nth 7 list))
(defun ninth   (list) (nth 8 list))
(defun tenth   (list) (nth 9 list))

;; cdddr and cddddr helpers
(defun cdddr  (x) (cdr (cddr x)))
(defun cddddr (x) (cdr (cdddr x)))
(defun caadr  (x) (car (cadr x)))
(defun cdadr  (x) (cdr (cadr x)))
(defun caaar  (x) (car (caar x)))
(defun caddar (x) (car (cdr (cdar x))))

;;;============================================================
;;; SECTION 5: LIST UTILITIES
;;;============================================================

(defun identity (x) x)

(defun complement (fn)
  (lambda (&rest args) (not (apply fn args))))

(defun constantly (value)
  (lambda (&rest args)
    (declare (ignore args))
    value))

(defun compose (&rest fns)
  (if (null fns) #'identity
      (let ((fn1 (car (last fns)))
            (rest (reverse (cdr (reverse fns)))))
        (lambda (&rest args)
          (reduce #'funcall rest
                  :initial-value (apply fn1 args))))))

;; ---- endp ----

(defun endp (x)
  "True if X is NIL (end of list)."
  (if (null x) t
      (if (consp x) nil
          (error "ENDP: not a list"))))

;; ---- acons / pairlis ----

(defun acons (key datum alist)
  "Add a (key . datum) pair to the front of ALIST."
  (cons (cons key datum) alist))

(defun pairlis (keys data &optional alist)
  "Construct an alist from KEYS and DATA."
  (if (null keys)
      alist
      (acons (car keys) (car data)
             (pairlis (cdr keys) (cdr data) alist))))

;; ---- copy-alist / copy-tree ----

(defun copy-alist (alist)
  "Copy the top-level spine and pairs of an alist."
  (mapcar (lambda (pair)
            (if (consp pair)
                (cons (car pair) (cdr pair))
                pair))
          alist))

(defun copy-tree (tree)
  "Recursively copy a tree of conses."
  (if (consp tree)
      (cons (copy-tree (car tree))
            (copy-tree (cdr tree)))
      tree))

;; ---- set operations ----

(defun intersection (list1 list2 &key (test #'eql))
  "Return elements common to both lists."
  (let ((result nil))
    (dolist (x list1 (reverse result))
      (when (member x list2)
        (push x result)))))

(defun union (list1 list2 &key (test #'eql))
  "Return the union of two lists."
  (let ((result (copy-list list2)))
    (dolist (x list1 result)
      (unless (member x result)
        (push x result)))))

(defun set-difference (list1 list2 &key (test #'eql))
  "Return elements in LIST1 not in LIST2."
  (let ((result nil))
    (dolist (x list1 (reverse result))
      (unless (member x list2)
        (push x result)))))

(defun set-exclusive-or (list1 list2 &key (test #'eql))
  "Return elements in exactly one of the two lists."
  (append (set-difference list1 list2)
          (set-difference list2 list1)))

(defun adjoin (item list &key (test #'eql))
  "Add ITEM to LIST if not already present."
  (if (member item list) list (cons item list)))

;; ---- sublis / subst ----

(defun subst (new old tree &key (test #'eql))
  "Substitute NEW for every occurrence of OLD in TREE."
  (cond
    ((funcall test old tree) new)
    ((consp tree)
     (let ((a (subst new old (car tree) :test test))
           (d (subst new old (cdr tree) :test test)))
       (if (and (eql a (car tree)) (eql d (cdr tree)))
           tree
           (cons a d))))
    (t tree)))

(defun sublis (alist tree &key (test #'eql))
  "Substitute from ALIST into TREE."
  (let ((pair (assoc tree alist)))
    (cond
      (pair (cdr pair))
      ((consp tree)
       (let ((a (sublis alist (car tree) :test test))
             (d (sublis alist (cdr tree) :test test)))
         (if (and (eql a (car tree)) (eql d (cdr tree)))
             tree
             (cons a d))))
      (t tree))))

;; ---- tree-equal ----

(defun tree-equal (a b &key (test #'eql))
  "Test two trees for structural equality."
  (cond
    ((and (consp a) (consp b))
     (and (tree-equal (car a) (car b) :test test)
          (tree-equal (cdr a) (cdr b) :test test)))
    ((and (atom a) (atom b))
     (funcall test a b))
    (t nil)))

;; ---- ldiff / tailp ----

(defun tailp (sublist list)
  "True if SUBLIST is a tail of LIST (by EQ)."
  (do ((l list (cdr l)))
      ((atom l) (eql l sublist))
    (when (eq l sublist) (return t))))

(defun ldiff (list sublist)
  "Return elements of LIST up to SUBLIST."
  (let ((result nil))
    (do ((l list (cdr l)))
        ((or (atom l) (eq l sublist)) (reverse result))
      (push (car l) result))))

;; ---- butlast / nbutlast ----

(defun butlast (list &optional (n 1))
  "Return a copy of LIST with the last N elements removed."
  (let* ((len (length list))
         (keep (- len n)))
    (if (<= keep 0)
        nil
        (let ((result nil))
          (dotimes (i keep (reverse result))
            (push (nth i list) result))))))

(defun nbutlast (list &optional (n 1))
  "Destructively remove the last N elements from LIST."
  (let* ((len (length list))
         (keep (- len n)))
    (if (<= keep 0)
        nil
        (progn
          (rplacd (nthcdr (1- keep) list) nil)
          list))))

;; ---- nreverse / nconc ----

(defun nreverse (list)
  "Destructively reverse LIST."
  (let ((prev nil) (curr list))
    (block nil
      (tagbody
       nrev-loop
       (when (null curr) (return prev))
       (let ((next (cdr curr)))
         (rplacd curr prev)
         (setq prev curr)
         (setq curr next))
       (go nrev-loop)))))

(defun nconc (&rest lists)
  "Destructively concatenate lists."
  (let ((result nil) (tail nil))
    (dolist (lst lists result)
      (when lst
        (if (null result)
            (progn (setq result lst)
                   (setq tail (last lst)))
            (progn (rplacd tail lst)
                   (setq tail (last lst))))))))

;; ---- iota / range / flatten / zip ----

(defun iota (n &optional (start 0) (step 1))
  "Generate a list of N numbers starting from START with STEP increment."
  (let ((result nil))
    (dotimes (i n (nreverse result))
      (push (+ start (* i step)) result))))

(defun range (start end &optional (step 1))
  (let ((result nil))
    (if (> step 0)
        (do ((i start (+ i step)))
            ((>= i end) (nreverse result))
          (push i result))
        (do ((i start (+ i step)))
            ((<= i end) (nreverse result))
          (push i result)))))

(defun flatten (tree)
  (cond
    ((null tree) nil)
    ((atom tree) (list tree))
    (t (append (flatten (car tree))
               (flatten (cdr tree))))))

(defun zip (&rest lists)
  (apply #'mapcar #'list lists))

;;;============================================================
;;; SECTION 6: SEQUENCE OPERATIONS
;;;============================================================

;; ---- find / find-if / find-if-not ----

(defun find (item sequence &key (test #'eql) (key #'identity) from-end)
  "Find ITEM in SEQUENCE."
  (let ((seq (if from-end (reverse sequence) sequence)))
    (dolist (x seq nil)
      (when (funcall test item (funcall key x))
        (return x)))))

(defun find-if (predicate sequence &key (key #'identity))
  "Find the first element satisfying PREDICATE."
  (dolist (x sequence nil)
    (when (funcall predicate (funcall key x))
      (return x))))

(defun find-if-not (predicate sequence &key (key #'identity))
  "Find the first element not satisfying PREDICATE."
  (dolist (x sequence nil)
    (unless (funcall predicate (funcall key x))
      (return x))))

;; ---- position / position-if ----

(defun position (item sequence &key (test #'eql) (key #'identity))
  "Return the position of ITEM in SEQUENCE."
  (let ((i 0))
    (dolist (x sequence nil)
      (when (funcall test item (funcall key x))
        (return i))
      (incf i))))

(defun position-if (predicate sequence &key (key #'identity))
  "Return the position of the first element satisfying PREDICATE."
  (let ((i 0))
    (dolist (x sequence nil)
      (when (funcall predicate (funcall key x))
        (return i))
      (incf i))))

;; ---- count / count-if ----

(defun count (item sequence &key (test #'eql) (key #'identity))
  "Count occurrences of ITEM in SEQUENCE."
  (let ((n 0))
    (dolist (x sequence n)
      (when (funcall test item (funcall key x))
        (incf n)))))

(defun count-if (predicate sequence &key (key #'identity))
  "Count elements satisfying PREDICATE."
  (let ((n 0))
    (dolist (x sequence n)
      (when (funcall predicate (funcall key x))
        (incf n)))))

;; ---- remove / remove-if / remove-if-not are builtins ----
;; Define remove in terms of remove-if-not:

(defun remove (item sequence &key (test #'eql) (key #'identity))
  "Return SEQUENCE with elements matching ITEM removed."
  (remove-if (lambda (x) (funcall test item (funcall key x))) sequence))

;; ---- substitute / substitute-if ----

(defun substitute (new old sequence &key (test #'eql) (key #'identity))
  "Return SEQUENCE with OLD replaced by NEW."
  (mapcar (lambda (x)
            (if (funcall test old (funcall key x)) new x))
          sequence))

(defun substitute-if (new predicate sequence &key (key #'identity))
  "Return SEQUENCE with elements satisfying PREDICATE replaced by NEW."
  (mapcar (lambda (x)
            (if (funcall predicate (funcall key x)) new x))
          sequence))

;; ---- search (subsequence search) ----

(defun search (subseq mainseq &key (test #'eql))
  "Search for SUBSEQ in MAINSEQ. Returns position or NIL."
  (let ((sublen (length subseq))
        (mainlen (length mainseq)))
    (if (= sublen 0) (return 0))
    (dotimes (i (1+ (- mainlen sublen)) nil)
      (block found
        (dotimes (j sublen)
          (unless (funcall test (nth j subseq) (nth (+ i j) mainseq))
            (return-from found nil)))
        (return i)))))

;; ---- mismatch ----

(defun mismatch (seq1 seq2 &key (test #'eql))
  "Return the position of the first mismatch between SEQ1 and SEQ2."
  (let ((len (min (length seq1) (length seq2))))
    (dotimes (i len
              (if (= (length seq1) (length seq2)) nil len))
      (unless (funcall test (nth i seq1) (nth i seq2))
        (return i)))))

;; ---- additional map functions ----
;; mapcar and mapc are builtins

(defun maplist (fn list)
  "Apply FN to successive sublists of LIST."
  (let ((result nil) (l list))
    (block nil
      (tagbody
       ml-loop
       (when (null l) (return (nreverse result)))
       (push (funcall fn l) result)
       (setq l (cdr l))
       (go ml-loop)))))

(defun mapl (fn list)
  "Like MAPLIST but returns the original list."
  (let ((l list))
    (block nil
      (tagbody
       ml-loop
       (when (null l) (return list))
       (funcall fn l)
       (setq l (cdr l))
       (go ml-loop)))))

(defun mapcan (fn list)
  "Like MAPCAR but uses NCONC to combine results."
  (let ((result nil))
    (dolist (x list result)
      (let ((r (funcall fn x)))
        (when r (setq result (nconc result r)))))))

(defun mapcon (fn list)
  "Like MAPLIST but uses NCONC to combine results."
  (let ((result nil) (l list))
    (block nil
      (tagbody
       mc-loop
       (when (null l) (return result))
       (let ((r (funcall fn l)))
         (when r (setq result (nconc result r))))
       (setq l (cdr l))
       (go mc-loop)))))

;; ---- make-list / make-string / make-sequence ----

(defun make-list (size &key initial-element)
  "Create a list of SIZE elements."
  (let ((result nil))
    (dotimes (i size result)
      (push initial-element result))))

(defun make-string (size &key (initial-element #\Space))
  "Create a string of SIZE characters."
  (let ((result (make-array size)))
    ;; Build via vector then coerce
    (dotimes (i size)
      (setf (aref result i) initial-element))
    (coerce result 'string)))

(defun make-sequence (type size &key initial-element)
  "Create a sequence of the given TYPE and SIZE."
  (cond
    ((eq type 'list)   (make-list size :initial-element initial-element))
    ((eq type 'vector) (let ((v (make-array size)))
                         (dotimes (i size v)
                           (setf (aref v i) initial-element))))
    ((eq type 'string) (make-string size :initial-element (or initial-element #\Space)))
    (t (error (format nil "make-sequence: unsupported type ~A" type)))))

;;;============================================================
;;; SECTION 7: STRING FUNCTIONS
;;;============================================================

;; string-trim already exists in the original, redefine cleanly:
(defun string-trim (chars string)
  "Remove leading and trailing characters in CHARS from STRING."
  (let* ((s (if (stringp string) string (string string)))
         (char-list (if (listp chars)
                        chars
                        (coerce chars 'list)))
         (len (length s))
         (start 0)
         (end len))
    (block nil
      (tagbody
       fwd
       (when (>= start end) (return ""))
       (when (not (member (char s start) char-list)) (go back))
       (incf start)
       (go fwd)
       back
       (when (<= end start) (return ""))
       (when (not (member (char s (1- end)) char-list)) (go done))
       (decf end)
       (go back)
       done))
    (subseq s start end)))

(defun string-left-trim (chars string)
  "Remove leading characters in CHARS from STRING."
  (let* ((s (if (stringp string) string (string string)))
         (char-list (if (listp chars) chars (coerce chars 'list)))
         (len (length s))
         (start 0))
    (block nil
      (tagbody
       fwd
       (when (>= start len) (return ""))
       (when (not (member (char s start) char-list)) (return (subseq s start len)))
       (incf start)
       (go fwd)))))

(defun string-right-trim (chars string)
  "Remove trailing characters in CHARS from STRING."
  (let* ((s (if (stringp string) string (string string)))
         (char-list (if (listp chars) chars (coerce chars 'list)))
         (end (length s)))
    (block nil
      (tagbody
       back
       (when (<= end 0) (return ""))
       (when (not (member (char s (1- end)) char-list)) (return (subseq s 0 end)))
       (decf end)
       (go back)))))

(defun string-split (string sep)
  "Split STRING by character SEP."
  (let ((result nil) (start 0) (len (length string)))
    (dotimes (i len)
      (when (eql (char string i) sep)
        (push (subseq string start i) result)
        (setq start (1+ i))))
    (push (subseq string start len) result)
    (nreverse result)))

(defun string-join (strings sep)
  "Join list of STRINGS with separator SEP."
  (if (null strings) ""
      (reduce (lambda (a b) (concatenate a sep b)) strings)))

;; ---- String comparison functions ----

(defun string< (s1 s2)
  "True if string S1 is lexicographically less than S2."
  (let* ((str1 (string s1)) (str2 (string s2))
         (len1 (length str1)) (len2 (length str2))
         (minlen (min len1 len2)))
    (dotimes (i minlen
              (if (< len1 len2) i nil))
      (let ((c1 (char-code (char str1 i)))
            (c2 (char-code (char str2 i))))
        (cond ((< c1 c2) (return i))
              ((> c1 c2) (return nil)))))))

(defun string> (s1 s2)
  "True if string S1 is lexicographically greater than S2."
  (string< s2 s1))

(defun string<= (s1 s2)
  "True if string S1 is lexicographically <= S2."
  (not (string> s1 s2)))

(defun string>= (s1 s2)
  "True if string S1 is lexicographically >= S2."
  (not (string< s1 s2)))

(defun string/= (s1 s2)
  "True if strings are not equal."
  (not (string= s1 s2)))

;; ---- Case-insensitive string comparisons ----

(defun string-equal (s1 s2)
  "Case-insensitive string equality."
  (string= (string-upcase (string s1)) (string-upcase (string s2))))

(defun string-lessp (s1 s2)
  "Case-insensitive string<."
  (string< (string-upcase (string s1)) (string-upcase (string s2))))

(defun string-greaterp (s1 s2)
  "Case-insensitive string>."
  (string> (string-upcase (string s1)) (string-upcase (string s2))))

;; ---- Character functions ----

(defun char-upcase (c)
  "Return the uppercase version of character C."
  (let ((code (char-code c)))
    (if (and (>= code 97) (<= code 122))  ;; a-z
        (code-char (- code 32))
        c)))

(defun char-downcase (c)
  "Return the lowercase version of character C."
  (let ((code (char-code c)))
    (if (and (>= code 65) (<= code 90))  ;; A-Z
        (code-char (+ code 32))
        c)))

(defun alpha-char-p (c)
  "True if C is an alphabetic character."
  (let ((code (char-code c)))
    (or (and (>= code 65) (<= code 90))
        (and (>= code 97) (<= code 122)))))

(defun upper-case-p (c)
  "True if C is uppercase."
  (let ((code (char-code c)))
    (and (>= code 65) (<= code 90))))

(defun lower-case-p (c)
  "True if C is lowercase."
  (let ((code (char-code c)))
    (and (>= code 97) (<= code 122))))

(defun digit-char-p (c &optional (radix 10))
  "Return the numeric weight of C or NIL."
  (let ((code (char-code c)))
    (cond
      ((and (>= code 48) (<= code 57))  ;; 0-9
       (let ((w (- code 48)))
         (if (< w radix) w nil)))
      ((and (>= code 65) (<= code 90))  ;; A-Z
       (let ((w (+ 10 (- code 65))))
         (if (< w radix) w nil)))
      ((and (>= code 97) (<= code 122)) ;; a-z
       (let ((w (+ 10 (- code 97))))
         (if (< w radix) w nil)))
      (t nil))))

(defun digit-char (weight &optional (radix 10))
  "Return the character for digit WEIGHT in given RADIX."
  (cond
    ((< weight 0) nil)
    ((>= weight radix) nil)
    ((< weight 10) (code-char (+ weight 48)))
    (t (code-char (+ weight 55)))))  ;; A=65, 10+55=65

(defun char= (c1 c2)  (eql c1 c2))
(defun char< (c1 c2)  (< (char-code c1) (char-code c2)))
(defun char> (c1 c2)  (> (char-code c1) (char-code c2)))
(defun char<= (c1 c2) (<= (char-code c1) (char-code c2)))
(defun char>= (c1 c2) (>= (char-code c1) (char-code c2)))

;;;============================================================
;;; SECTION 8: MULTIPLE VALUES
;;;============================================================

;; values is a builtin

(defun values-list (list)
  "Return elements of LIST as multiple values."
  (apply #'values list))

(defmacro multiple-value-list (form)
  "Evaluate FORM and return all its values as a list."
  (let ((g (gensym)))
    ;; We rely on the runtime MV mechanism.
    ;; After evaluating the form, multiple-value-list should capture all values.
    ;; Since we lack a direct MV capture primitive, simulate it:
    `(let ((,g (multiple-value-call #'list ,form)))
       ,g)))

(defmacro nth-value (n form)
  "Return the Nth value of FORM."
  `(nth ,n (multiple-value-list ,form)))

;;;============================================================
;;; SECTION 9: CONDITION SYSTEM WRAPPERS
;;;============================================================

;; The C runtime provides: %handler-case, %handler-bind, %restart-case,
;; %restart-bind, signal, cerror, make-condition, find-restart,
;; invoke-restart, compute-restarts.

(defmacro handler-case (form &rest clauses)
  "Handle conditions raised during evaluation of FORM."
  (if (null clauses)
      form
      (let ((clause (car clauses))
            (rest-clauses (cdr clauses)))
        (let ((type (car clause))
              (lambda-list (cadr clause))
              (body (cddr clause)))
          (if (eq type :no-error)
              ;; :no-error clause
              `(let ((%hc-result ,form))
                 (funcall (lambda ,lambda-list ,@body) %hc-result))
              `(%handler-case
                (find-class ',type)
                (lambda ()
                  ,(if rest-clauses
                       `(handler-case ,form ,@rest-clauses)
                       form))
                (lambda ,lambda-list ,@body)))))))

(defmacro handler-bind (bindings &body body)
  "Bind condition handlers during evaluation of BODY."
  (if (null bindings)
      `(progn ,@body)
      (let ((handlers-list
             (mapcar (lambda (binding)
                       `(cons (find-class ',(car binding)) ,(cadr binding)))
                     bindings)))
        `(%handler-bind
          (list ,@handlers-list)
          (lambda () ,@body)))))

(defmacro restart-case (form &rest clauses)
  "Establish restarts during evaluation of FORM."
  (let ((restart-specs
         (mapcar (lambda (clause)
                   (let ((name (car clause))
                         (lambda-list (cadr clause))
                         (body (cddr clause)))
                     `(list ',name (lambda ,lambda-list ,@body))))
                 clauses)))
    `(%restart-case
      (list ,@restart-specs)
      (lambda () ,form))))

(defmacro ignore-errors (&body body)
  "Execute BODY, returning NIL and the condition if an error occurs."
  `(handler-case (progn ,@body)
     (error (c) (values nil c))))

(defmacro assert (test-form &optional places datum &rest args)
  "Signal an error if TEST-FORM evaluates to NIL."
  (if datum
      `(unless ,test-form
         (error ,datum ,@args))
      `(unless ,test-form
         (error (format nil "Assertion failed: ~S" ',test-form)))))

(defmacro check-type (place typespec &optional string)
  "Signal a type error if PLACE is not of type TYPESPEC."
  `(unless (typep ,place ',typespec)
     (error ,(or string
                 (format nil "The value of ~A is not of type ~A"
                         place typespec)))))

;;;============================================================
;;; SECTION 10: DEFINITION MACROS
;;;============================================================

;; ---- defstruct ----

(defmacro defstruct (name-and-options &rest slot-descs)
  "Define a structure type.
   (defstruct name slot1 (slot2 default2) ...)
   (defstruct (name (:conc-name PREFIX-) (:constructor MAKE-NAME)) ...)
   Generates: make-NAME, NAME-p, copy-NAME, NAME-SLOT accessors."
  (let* ((name (if (listp name-and-options)
                   (car name-and-options)
                   name-and-options))
         (options (if (listp name-and-options) (cdr name-and-options) nil))
         (name-str (symbol-name name))
         ;; Parse :conc-name option
         (conc-name
          (let ((opt (assoc :conc-name options)))
            (cond
              ((null opt) (concatenate name-str "-"))
              ((null (cdr opt)) "")  ;; (:conc-name) means no prefix
              ((null (cadr opt)) "")  ;; (:conc-name nil)
              (t (symbol-name (cadr opt))))))
         ;; Parse :constructor option
         (constructor-name
          (let ((opt (assoc :constructor options)))
            (if (and opt (cdr opt))
                (cadr opt)
                (intern (concatenate "MAKE-" name-str)))))
         ;; Parse :predicate option
         (predicate-name
          (let ((opt (assoc :predicate options)))
            (if (and opt (cdr opt))
                (cadr opt)
                (intern (concatenate name-str "-P")))))
         ;; Parse :copier option
         (copier-name
          (let ((opt (assoc :copier options)))
            (if (and opt (cdr opt))
                (cadr opt)
                (intern (concatenate "COPY-" name-str)))))
         ;; Parse slot descriptions
         (slots (mapcar (lambda (sd)
                          (if (listp sd)
                              (list (car sd) (cadr sd))  ;; (name default)
                              (list sd nil)))             ;; name with nil default
                        slot-descs))
         (slot-names (mapcar #'car slots))
         (slot-defaults (mapcar #'cadr slots)))
    `(progn
       ;; Constructor: (make-NAME &key slot1 slot2 ...)
       (defun ,constructor-name (&key ,@(mapcar (lambda (s)
                                                  (list (car s) (cadr s)))
                                                slots))
         (list ',name ,@slot-names))

       ;; Predicate: (NAME-p obj)
       (defun ,predicate-name (obj)
         (and (listp obj) (eq (car obj) ',name)))

       ;; Copier: (copy-NAME obj)
       (defun ,copier-name (obj)
         (copy-list obj))

       ;; Accessors: (NAME-SLOT obj), (setf (NAME-SLOT obj) val)
       ,@(let ((accessor-defs nil)
               (idx 1))
           (dolist (sname slot-names (nreverse accessor-defs))
             (let ((acc-name (intern (concatenate conc-name
                                                  (symbol-name sname)))))
               (push `(defun ,acc-name (obj) (nth ,idx obj)) accessor-defs)
               ;; Register a setf expander
               (push `(%register-setf-expander ',acc-name
                        (lambda (place-args new-val)
                          (list 'rplaca
                                (list 'nthcdr ,idx (car place-args))
                                new-val)))
                     accessor-defs))
             (incf idx)))

       ',name)))

;; ---- deftype (basic) ----

(defvar *type-definitions* nil)

(defmacro deftype (name lambda-list &body body)
  "Define a type specifier (simplified)."
  `(progn
     (push (cons ',name (lambda ,lambda-list ,@body)) *type-definitions*)
     ',name))

;;;============================================================
;;; SECTION 11: THE LOOP MACRO
;;;============================================================

;;; This is a comprehensive implementation of the CL LOOP macro.
;;; It supports the extended loop syntax with the following clauses:
;;;   for/as VAR from X to/below/above Y [by Z]
;;;   for/as VAR in LIST [by fn]
;;;   for/as VAR across VECTOR
;;;   for/as VAR = EXPR [then EXPR]
;;;   for/as VAR on LIST [by fn]
;;;   with VAR = EXPR
;;;   repeat N
;;;   while COND
;;;   until COND
;;;   do/doing FORM...
;;;   collect/collecting FORM [into var]
;;;   append/appending FORM [into var]
;;;   nconc/nconcing FORM [into var]
;;;   sum/summing FORM [into var]
;;;   count/counting FORM [into var]
;;;   maximize/maximizing FORM [into var]
;;;   minimize/minimizing FORM [into var]
;;;   always FORM
;;;   never FORM
;;;   thereis FORM
;;;   return FORM
;;;   finally FORM...
;;;   initially FORM...

(defun %loop-keyword-p (sym)
  "Check if SYM is a loop keyword."
  (and (symbolp sym)
       (member (symbol-name sym)
               '("FOR" "AS" "WITH" "DO" "DOING" "COLLECT" "COLLECTING"
                 "APPEND" "APPENDING" "NCONC" "NCONCING"
                 "SUM" "SUMMING" "COUNT" "COUNTING"
                 "MAXIMIZE" "MAXIMIZING" "MINIMIZE" "MINIMIZING"
                 "REPEAT" "WHILE" "UNTIL"
                 "ALWAYS" "NEVER" "THEREIS"
                 "INITIALLY" "FINALLY" "RETURN"
                 "FROM" "DOWNFROM" "UPFROM" "TO" "UPTO" "DOWNTO"
                 "BELOW" "ABOVE" "BY" "IN" "ON" "ACROSS" "BEING"
                 "THEN" "=" "INTO" "IF" "WHEN" "UNLESS" "ELSE" "END"
                 "NAMED")
               :test #'string=)))

(defun %loop-sym-eq (sym name)
  "Case-insensitive comparison for loop keywords."
  (and (symbolp sym)
       (string= (symbol-name sym) name)))

(defmacro loop (&body body)
  "The extended LOOP macro."
  ;; Simple loop: if body is not keyword-driven, just loop forever
  (if (or (null body)
          (and (not (symbolp (car body)))
               (not (%loop-keyword-p (car body)))))
      ;; Simple (loop BODY...) -- infinite loop with block nil
      `(block nil
         (tagbody
          %loop-top
          ,@body
          (go %loop-top)))
      ;; Extended loop
      (%expand-loop body)))

(defun %expand-loop (body)
  "Parse and expand the extended LOOP syntax."
  (let (;; Accumulate these during clause parsing
        (bindings nil)         ;; let bindings: ((var init) ...)
        (pre-steps nil)        ;; forms to execute before the test
        (tests nil)            ;; termination tests (when any is true, stop)
        (body-forms nil)       ;; main body forms
        (post-steps nil)       ;; step forms after body
        (initially-forms nil)  ;; initially clause forms
        (finally-forms nil)    ;; finally clause forms
        (result-form nil)      ;; the value to return (set by accumulation vars)
        (accum-vars nil)       ;; accumulation variable bindings
        (loop-name nil)        ;; optional block name
        (always-flag nil)      ;; gensym for always/never tracking
        (clauses body))        ;; remaining clauses to parse

    ;; Check for NAMED
    (when (and clauses (%loop-sym-eq (car clauses) "NAMED"))
      (pop clauses)
      (setq loop-name (pop clauses)))

    ;; Parse clauses
    (block parse-done
      (tagbody
       parse-loop
       (when (null clauses) (return-from parse-done nil))

       (let ((kw (pop clauses)))
         (cond
           ;; ---- FOR / AS ----
           ((or (%loop-sym-eq kw "FOR") (%loop-sym-eq kw "AS"))
            (let ((var (pop clauses))
                  (prep (car clauses)))
              (cond
                ;; FOR var FROM x TO y [BY z]
                ((or (%loop-sym-eq prep "FROM")
                     (%loop-sym-eq prep "UPFROM")
                     (%loop-sym-eq prep "DOWNFROM"))
                 (let ((direction (cond ((%loop-sym-eq prep "DOWNFROM") 'down)
                                        (t 'up)))
                       (from-val nil)
                       (to-val nil)
                       (to-type nil)  ;; 'to or 'below or 'above
                       (by-val nil))
                   (pop clauses) ;; consume FROM/UPFROM/DOWNFROM
                   (setq from-val (pop clauses))
                   ;; Parse TO/BELOW/ABOVE
                   (when (and clauses
                              (or (%loop-sym-eq (car clauses) "TO")
                                  (%loop-sym-eq (car clauses) "UPTO")
                                  (%loop-sym-eq (car clauses) "DOWNTO")
                                  (%loop-sym-eq (car clauses) "BELOW")
                                  (%loop-sym-eq (car clauses) "ABOVE")))
                     (let ((tk (pop clauses)))
                       (setq to-type (cond ((%loop-sym-eq tk "TO") 'to)
                                           ((%loop-sym-eq tk "UPTO") 'to)
                                           ((%loop-sym-eq tk "DOWNTO")
                                            (setq direction 'down) 'to)
                                           ((%loop-sym-eq tk "BELOW") 'below)
                                           ((%loop-sym-eq tk "ABOVE")
                                            (setq direction 'down) 'above)))
                       (setq to-val (pop clauses))))
                   ;; Parse BY
                   (when (and clauses (%loop-sym-eq (car clauses) "BY"))
                     (pop clauses)
                     (setq by-val (pop clauses)))
                   ;; Generate code
                   (let ((limit-var (when to-val (gensym)))
                         (step-var (when by-val (gensym))))
                     (push (list var from-val) bindings)
                     (when limit-var (push (list limit-var to-val) bindings))
                     (when step-var (push (list step-var by-val) bindings))
                     ;; Termination test
                     (when to-val
                       (let ((step-amount (or step-var (or by-val 1))))
                         (if (eq direction 'down)
                             (push (if (eq to-type 'above)
                                       `(<= ,var ,limit-var)
                                       `(< ,var ,limit-var))
                                   tests)
                             (push (if (eq to-type 'below)
                                       `(>= ,var ,limit-var)
                                       `(> ,var ,limit-var))
                                   tests))))
                     ;; Step
                     (let ((step-amount (or step-var (or by-val 1))))
                       (push `(setq ,var ,(if (eq direction 'down)
                                              `(- ,var ,step-amount)
                                              `(+ ,var ,step-amount)))
                             post-steps)))))

                ;; FOR var = expr [THEN expr]
                ((%loop-sym-eq prep "=")
                 (pop clauses) ;; consume =
                 (let ((init-expr (pop clauses)))
                   (if (and clauses (%loop-sym-eq (car clauses) "THEN"))
                       (progn
                         (pop clauses) ;; consume THEN
                         (let ((step-expr (pop clauses)))
                           (push (list var init-expr) bindings)
                           (push `(setq ,var ,step-expr) post-steps)))
                       (progn
                         (push (list var init-expr) bindings)
                         (push `(setq ,var ,init-expr) post-steps)))))

                ;; FOR var IN list [BY fn]
                ((%loop-sym-eq prep "IN")
                 (pop clauses) ;; consume IN
                 (let ((list-expr (pop clauses))
                       (step-fn nil)
                       (list-var (gensym)))
                   (when (and clauses (%loop-sym-eq (car clauses) "BY"))
                     (pop clauses)
                     (setq step-fn (pop clauses)))
                   (push (list list-var list-expr) bindings)
                   (push (list var nil) bindings)
                   ;; Termination
                   (push `(null ,list-var) tests)
                   ;; Before body: set var
                   (push `(setq ,var (car ,list-var)) pre-steps)
                   ;; Step
                   (push `(setq ,list-var ,(if step-fn
                                               `(funcall ,step-fn ,list-var)
                                               `(cdr ,list-var)))
                         post-steps)))

                ;; FOR var ON list [BY fn]
                ((%loop-sym-eq prep "ON")
                 (pop clauses) ;; consume ON
                 (let ((list-expr (pop clauses))
                       (step-fn nil))
                   (when (and clauses (%loop-sym-eq (car clauses) "BY"))
                     (pop clauses)
                     (setq step-fn (pop clauses)))
                   (push (list var list-expr) bindings)
                   ;; Termination
                   (push `(null ,var) tests)
                   ;; Step
                   (push `(setq ,var ,(if step-fn
                                          `(funcall ,step-fn ,var)
                                          `(cdr ,var)))
                         post-steps)))

                ;; FOR var ACROSS vector
                ((%loop-sym-eq prep "ACROSS")
                 (pop clauses) ;; consume ACROSS
                 (let ((vec-expr (pop clauses))
                       (vec-var (gensym))
                       (idx-var (gensym))
                       (len-var (gensym)))
                   (push (list vec-var vec-expr) bindings)
                   (push (list len-var `(length ,vec-var)) bindings)
                   (push (list idx-var 0) bindings)
                   (push (list var nil) bindings)
                   ;; Termination
                   (push `(>= ,idx-var ,len-var) tests)
                   ;; Before body
                   (push `(setq ,var (aref ,vec-var ,idx-var)) pre-steps)
                   ;; Step
                   (push `(incf ,idx-var) post-steps)))

                ;; FOR var with no prep -- treat as (FOR var = nil)
                (t
                 ;; Numeric from without explicit FROM keyword: FOR var TO y
                 (if (or (%loop-sym-eq prep "TO")
                         (%loop-sym-eq prep "UPTO")
                         (%loop-sym-eq prep "BELOW")
                         (%loop-sym-eq prep "DOWNTO")
                         (%loop-sym-eq prep "ABOVE"))
                     (progn
                       ;; Implicit FROM 0
                       (push `(for ,var from 0 ,prep ,@clauses) clauses)
                       ;; Actually just re-parse with explicit from
                       ;; Simpler: handle inline
                       (let ((to-type (cond ((%loop-sym-eq prep "TO") 'to)
                                            ((%loop-sym-eq prep "UPTO") 'to)
                                            ((%loop-sym-eq prep "BELOW") 'below)
                                            ((%loop-sym-eq prep "DOWNTO") 'to)
                                            ((%loop-sym-eq prep "ABOVE") 'above)))
                             (direction (if (or (%loop-sym-eq prep "DOWNTO")
                                                (%loop-sym-eq prep "ABOVE"))
                                            'down 'up)))
                         (pop clauses) ;; consume TO/BELOW/etc
                         (let ((to-val (pop clauses))
                               (by-val nil)
                               (limit-var (gensym)))
                           (when (and clauses (%loop-sym-eq (car clauses) "BY"))
                             (pop clauses)
                             (setq by-val (pop clauses)))
                           (push (list var 0) bindings)
                           (push (list limit-var to-val) bindings)
                           (let ((step-amount (or by-val 1)))
                             (if (eq direction 'down)
                                 (push (if (eq to-type 'above)
                                           `(<= ,var ,limit-var)
                                           `(< ,var ,limit-var))
                                       tests)
                                 (push (if (eq to-type 'below)
                                           `(>= ,var ,limit-var)
                                           `(> ,var ,limit-var))
                                       tests))
                             (push `(setq ,var ,(if (eq direction 'down)
                                                    `(- ,var ,step-amount)
                                                    `(+ ,var ,step-amount)))
                                   post-steps)))))
                     ;; Unknown FOR subclause
                     (progn
                       (push (list var nil) bindings)))))))

           ;; ---- WITH ----
           ((or (%loop-sym-eq kw "WITH"))
            (let ((var (pop clauses))
                  (init nil))
              (when (and clauses (%loop-sym-eq (car clauses) "="))
                (pop clauses) ;; consume =
                (setq init (pop clauses)))
              ;; Skip AND chains
              (block with-and-done
                (tagbody
                 with-and-loop
                 (when (and clauses (%loop-sym-eq (car clauses) "AND"))
                   (pop clauses) ;; consume AND
                   (let ((v2 (pop clauses))
                         (i2 nil))
                     (when (and clauses (%loop-sym-eq (car clauses) "="))
                       (pop clauses)
                       (setq i2 (pop clauses)))
                     (push (list v2 i2) bindings))
                   (go with-and-loop))))
              (push (list var init) bindings)))

           ;; ---- REPEAT ----
           ((%loop-sym-eq kw "REPEAT")
            (let ((count-expr (pop clauses))
                  (count-var (gensym)))
              (push (list count-var count-expr) bindings)
              (push `(<= ,count-var 0) tests)
              (push `(decf ,count-var) post-steps)))

           ;; ---- WHILE ----
           ((%loop-sym-eq kw "WHILE")
            (let ((test-expr (pop clauses)))
              (push `(not ,test-expr) tests)))

           ;; ---- UNTIL ----
           ((%loop-sym-eq kw "UNTIL")
            (let ((test-expr (pop clauses)))
              (push test-expr tests)))

           ;; ---- DO / DOING ----
           ((or (%loop-sym-eq kw "DO") (%loop-sym-eq kw "DOING"))
            ;; Collect body forms until next loop keyword
            (block do-done
              (tagbody
               do-loop
               (when (or (null clauses)
                         (%loop-keyword-p (car clauses)))
                 (return-from do-done nil))
               (push (pop clauses) body-forms)
               (go do-loop))))

           ;; ---- COLLECT / COLLECTING ----
           ((or (%loop-sym-eq kw "COLLECT") (%loop-sym-eq kw "COLLECTING"))
            (let ((expr (pop clauses))
                  (into-var nil))
              (when (and clauses (%loop-sym-eq (car clauses) "INTO"))
                (pop clauses)
                (setq into-var (pop clauses)))
              (let* ((acc (or into-var
                             (let ((existing (assoc :collect accum-vars)))
                               (if existing (cdr existing)
                                   (let ((g (gensym)))
                                     (push (cons :collect g) accum-vars)
                                     g)))))
                     (tail-var (gensym)))
                (unless (assoc acc bindings :test #'eq)
                  (push (list acc nil) bindings)
                  (push (list tail-var nil) bindings))
                (push `(let ((%loop-cell (list ,expr)))
                         (if (null ,acc)
                             (progn (setq ,acc %loop-cell)
                                    (setq ,tail-var %loop-cell))
                             (progn (rplacd ,tail-var %loop-cell)
                                    (setq ,tail-var %loop-cell))))
                      body-forms)
                (unless into-var
                  (setq result-form acc)))))

           ;; ---- APPEND / APPENDING ----
           ((or (%loop-sym-eq kw "APPEND") (%loop-sym-eq kw "APPENDING"))
            (let ((expr (pop clauses))
                  (into-var nil))
              (when (and clauses (%loop-sym-eq (car clauses) "INTO"))
                (pop clauses)
                (setq into-var (pop clauses)))
              (let ((acc (or into-var
                            (let ((existing (assoc :append accum-vars)))
                              (if existing (cdr existing)
                                  (let ((g (gensym)))
                                    (push (cons :append g) accum-vars)
                                    g))))))
                (unless (assoc acc bindings :test #'eq)
                  (push (list acc nil) bindings))
                (push `(setq ,acc (append ,acc (copy-list ,expr))) body-forms)
                (unless into-var
                  (setq result-form acc)))))

           ;; ---- NCONC / NCONCING ----
           ((or (%loop-sym-eq kw "NCONC") (%loop-sym-eq kw "NCONCING"))
            (let ((expr (pop clauses))
                  (into-var nil))
              (when (and clauses (%loop-sym-eq (car clauses) "INTO"))
                (pop clauses)
                (setq into-var (pop clauses)))
              (let ((acc (or into-var
                            (let ((existing (assoc :nconc accum-vars)))
                              (if existing (cdr existing)
                                  (let ((g (gensym)))
                                    (push (cons :nconc g) accum-vars)
                                    g))))))
                (unless (assoc acc bindings :test #'eq)
                  (push (list acc nil) bindings))
                (push `(setq ,acc (nconc ,acc ,expr)) body-forms)
                (unless into-var
                  (setq result-form acc)))))

           ;; ---- SUM / SUMMING ----
           ((or (%loop-sym-eq kw "SUM") (%loop-sym-eq kw "SUMMING"))
            (let ((expr (pop clauses))
                  (into-var nil))
              (when (and clauses (%loop-sym-eq (car clauses) "INTO"))
                (pop clauses)
                (setq into-var (pop clauses)))
              (let ((acc (or into-var
                            (let ((existing (assoc :sum accum-vars)))
                              (if existing (cdr existing)
                                  (let ((g (gensym)))
                                    (push (cons :sum g) accum-vars)
                                    g))))))
                (unless (assoc acc bindings :test #'eq)
                  (push (list acc 0) bindings))
                (push `(incf ,acc ,expr) body-forms)
                (unless into-var
                  (setq result-form acc)))))

           ;; ---- COUNT / COUNTING ----
           ((or (%loop-sym-eq kw "COUNT") (%loop-sym-eq kw "COUNTING"))
            (let ((expr (pop clauses))
                  (into-var nil))
              (when (and clauses (%loop-sym-eq (car clauses) "INTO"))
                (pop clauses)
                (setq into-var (pop clauses)))
              (let ((acc (or into-var
                            (let ((existing (assoc :count accum-vars)))
                              (if existing (cdr existing)
                                  (let ((g (gensym)))
                                    (push (cons :count g) accum-vars)
                                    g))))))
                (unless (assoc acc bindings :test #'eq)
                  (push (list acc 0) bindings))
                (push `(when ,expr (incf ,acc)) body-forms)
                (unless into-var
                  (setq result-form acc)))))

           ;; ---- MAXIMIZE / MAXIMIZING ----
           ((or (%loop-sym-eq kw "MAXIMIZE") (%loop-sym-eq kw "MAXIMIZING"))
            (let ((expr (pop clauses))
                  (into-var nil))
              (when (and clauses (%loop-sym-eq (car clauses) "INTO"))
                (pop clauses)
                (setq into-var (pop clauses)))
              (let ((acc (or into-var
                            (let ((existing (assoc :max accum-vars)))
                              (if existing (cdr existing)
                                  (let ((g (gensym)))
                                    (push (cons :max g) accum-vars)
                                    g))))))
                (unless (assoc acc bindings :test #'eq)
                  (push (list acc nil) bindings))
                (let ((tmp (gensym)))
                  (push `(let ((,tmp ,expr))
                           (when (or (null ,acc) (> ,tmp ,acc))
                             (setq ,acc ,tmp)))
                        body-forms))
                (unless into-var
                  (setq result-form acc)))))

           ;; ---- MINIMIZE / MINIMIZING ----
           ((or (%loop-sym-eq kw "MINIMIZE") (%loop-sym-eq kw "MINIMIZING"))
            (let ((expr (pop clauses))
                  (into-var nil))
              (when (and clauses (%loop-sym-eq (car clauses) "INTO"))
                (pop clauses)
                (setq into-var (pop clauses)))
              (let ((acc (or into-var
                            (let ((existing (assoc :min accum-vars)))
                              (if existing (cdr existing)
                                  (let ((g (gensym)))
                                    (push (cons :min g) accum-vars)
                                    g))))))
                (unless (assoc acc bindings :test #'eq)
                  (push (list acc nil) bindings))
                (let ((tmp (gensym)))
                  (push `(let ((,tmp ,expr))
                           (when (or (null ,acc) (< ,tmp ,acc))
                             (setq ,acc ,tmp)))
                        body-forms))
                (unless into-var
                  (setq result-form acc)))))

           ;; ---- ALWAYS ----
           ((%loop-sym-eq kw "ALWAYS")
            (let ((expr (pop clauses))
                  (flag (or always-flag
                            (let ((g (gensym)))
                              (setq always-flag g)
                              (push (list g t) bindings)
                              g))))
              (push `(unless ,expr
                       (setq ,flag nil)
                       (return nil))
                    body-forms)
              (setq result-form flag)))

           ;; ---- NEVER ----
           ((%loop-sym-eq kw "NEVER")
            (let ((expr (pop clauses))
                  (flag (or always-flag
                            (let ((g (gensym)))
                              (setq always-flag g)
                              (push (list g t) bindings)
                              g))))
              (push `(when ,expr
                       (setq ,flag nil)
                       (return nil))
                    body-forms)
              (setq result-form flag)))

           ;; ---- THEREIS ----
           ((%loop-sym-eq kw "THEREIS")
            (let ((expr (pop clauses))
                  (tmp (gensym)))
              (push `(let ((,tmp ,expr))
                       (when ,tmp (return ,tmp)))
                    body-forms)))

           ;; ---- RETURN ----
           ((%loop-sym-eq kw "RETURN")
            (let ((expr (pop clauses)))
              (push `(return ,expr) body-forms)))

           ;; ---- INITIALLY ----
           ((%loop-sym-eq kw "INITIALLY")
            (block init-done
              (tagbody
               init-loop
               (when (or (null clauses)
                         (%loop-keyword-p (car clauses)))
                 (return-from init-done nil))
               (push (pop clauses) initially-forms)
               (go init-loop))))

           ;; ---- FINALLY ----
           ((%loop-sym-eq kw "FINALLY")
            (block fin-done
              (tagbody
               fin-loop
               (when (or (null clauses)
                         (%loop-keyword-p (car clauses)))
                 (return-from fin-done nil))
               (push (pop clauses) finally-forms)
               (go fin-loop))))

           ;; ---- IF / WHEN / UNLESS in loop ----
           ;; Strategy: build the conditional body inline by invoking the same
           ;; accumulator construction used by top-level COLLECT/SUM/COUNT/DO.
           ((or (%loop-sym-eq kw "IF") (%loop-sym-eq kw "WHEN")
                (%loop-sym-eq kw "UNLESS"))
            (let ((negate (%loop-sym-eq kw "UNLESS"))
                  (test-expr (pop clauses))
                  (then-stmts nil)
                  (else-stmts nil)
                  (target :then))
              (labels
                  ((emit (s)
                     (if (eq target :then)
                         (push s then-stmts)
                         (push s else-stmts)))
                   (parse-one ()
                     ;; Parse exactly one sub-clause and append generated stmt(s).
                     (let ((sub-kw (pop clauses)))
                       (cond
                         ((or (%loop-sym-eq sub-kw "DO")
                              (%loop-sym-eq sub-kw "DOING"))
                          (emit (pop clauses)))
                         ((or (%loop-sym-eq sub-kw "COLLECT")
                              (%loop-sym-eq sub-kw "COLLECTING"))
                          (let* ((expr (pop clauses))
                                 (acc (let ((ex (assoc :collect accum-vars)))
                                        (if ex (cdr ex)
                                            (let ((g (gensym)))
                                              (push (cons :collect g) accum-vars)
                                              g))))
                                 (tail-var (let ((ex (assoc :collect-tail accum-vars)))
                                             (if ex (cdr ex)
                                                 (let ((g (gensym)))
                                                   (push (cons :collect-tail g) accum-vars)
                                                   g)))))
                            (unless (assoc acc bindings :test #'eq)
                              (push (list acc nil) bindings)
                              (push (list tail-var nil) bindings))
                            (setq result-form acc)
                            (emit `(let ((%loop-cell (list ,expr)))
                                     (if (null ,acc)
                                         (progn (setq ,acc %loop-cell)
                                                (setq ,tail-var %loop-cell))
                                         (progn (rplacd ,tail-var %loop-cell)
                                                (setq ,tail-var %loop-cell)))))))
                         ((or (%loop-sym-eq sub-kw "SUM")
                              (%loop-sym-eq sub-kw "SUMMING"))
                          (let* ((expr (pop clauses))
                                 (acc (let ((ex (assoc :sum accum-vars)))
                                        (if ex (cdr ex)
                                            (let ((g (gensym)))
                                              (push (cons :sum g) accum-vars)
                                              g)))))
                            (unless (assoc acc bindings :test #'eq)
                              (push (list acc 0) bindings))
                            (setq result-form acc)
                            (emit `(incf ,acc ,expr))))
                         ((or (%loop-sym-eq sub-kw "COUNT")
                              (%loop-sym-eq sub-kw "COUNTING"))
                          (let* ((expr (pop clauses))
                                 (acc (let ((ex (assoc :count accum-vars)))
                                        (if ex (cdr ex)
                                            (let ((g (gensym)))
                                              (push (cons :count g) accum-vars)
                                              g)))))
                            (unless (assoc acc bindings :test #'eq)
                              (push (list acc 0) bindings))
                            (setq result-form acc)
                            (emit `(when ,expr (incf ,acc)))))
                         ((%loop-sym-eq sub-kw "RETURN")
                          (emit `(return ,(pop clauses))))
                         (t
                          (emit sub-kw)))
                       (when (and clauses (%loop-sym-eq (car clauses) "AND"))
                         (pop clauses)
                         (parse-one)))))
                ;; Parse THEN branch
                (block then-done
                  (tagbody
                   then-loop
                   (when (or (null clauses)
                             (and (%loop-keyword-p (car clauses))
                                  (or (%loop-sym-eq (car clauses) "ELSE")
                                      (%loop-sym-eq (car clauses) "END"))))
                     (return-from then-done nil))
                   (parse-one)
                   (go then-loop)))
                ;; Optional ELSE branch
                (when (and clauses (%loop-sym-eq (car clauses) "ELSE"))
                  (pop clauses)
                  (setq target :else)
                  (block else-done
                    (tagbody
                     else-loop
                     (when (or (null clauses)
                               (and (%loop-keyword-p (car clauses))
                                    (%loop-sym-eq (car clauses) "END")))
                       (return-from else-done nil))
                     (parse-one)
                     (go else-loop))))
                (when (and clauses (%loop-sym-eq (car clauses) "END"))
                  (pop clauses)))
              (let ((effective-test (if negate `(not ,test-expr) test-expr)))
                (cond
                  (else-stmts
                   (push `(if ,effective-test
                              (progn ,@(nreverse then-stmts))
                              (progn ,@(nreverse else-stmts)))
                         body-forms))
                  (then-stmts
                   (push `(when ,effective-test ,@(nreverse then-stmts))
                         body-forms))))))

           ;; Unknown clause: treat as body form
           (t
            (push kw body-forms))))

       (go parse-loop)))

    ;; --- Generate the expansion ---
    (let* ((reversed-bindings (nreverse bindings))
           (all-body (append
                             ;; Termination tests run FIRST so out-of-bounds
                             ;; iterators (e.g. ACROSS) don't run their pre-step
                             ;; assignment with an invalid index.
                             (when tests
                               (list `(when (or ,@(nreverse tests))
                                        (go %loop-end))))
                             (nreverse pre-steps)
                             (nreverse body-forms)
                             (nreverse post-steps))))
      `(block ,(or loop-name nil)
         (let* ,reversed-bindings
           ,@(nreverse initially-forms)
           (tagbody
            %loop-top
            ,@all-body
            (go %loop-top)
            %loop-end)
           ,@(when finally-forms
               (nreverse finally-forms))
           ,result-form)))))


;;;============================================================
;;; SECTION 12: MISCELLANEOUS UTILITIES
;;;============================================================

;; ---- Math utilities ----

(defconstant +e+ 2.718281828459045d0)
(defconstant +phi+ 1.618033988749895d0)

(defun factorial (n)
  (if (<= n 1) 1 (* n (factorial (1- n)))))

(defun fibonacci (n)
  (if (< n 2) n
      (+ (fibonacci (- n 1)) (fibonacci (- n 2)))))

(defun gcd (a b)
  (if (zerop b) (abs a) (gcd b (mod a b))))

(defun lcm (a b)
  (if (or (zerop a) (zerop b)) 0
      (abs (/ (* a b) (gcd a b)))))

;; ---- I/O utilities ----

(defun println (x)
  (princ x)
  (terpri)
  x)

(defun prompt (message)
  (princ message)
  (read-line))

;; ---- write-to-string / prin1-to-string / princ-to-string ----

(defun write-to-string (object)
  "Return a string representation of OBJECT (as PRIN1)."
  (format nil "~S" object))

(defun prin1-to-string (object)
  "Return a string representation with escaping."
  (format nil "~S" object))

(defun princ-to-string (object)
  "Return a string representation without escaping."
  (format nil "~A" object))

;; ---- y-or-n-p / yes-or-no-p ----

(defun y-or-n-p (&optional format-string &rest args)
  "Ask a yes/no question. Returns T or NIL."
  (when format-string
    (apply #'format t format-string args)
    (princ " (y/n) "))
  (let ((response (read-line)))
    (cond
      ((or (string= response "y") (string= response "Y")) t)
      ((or (string= response "n") (string= response "N")) nil)
      (t (princ "Please answer y or n: ")
         (y-or-n-p)))))

(defun yes-or-no-p (&optional format-string &rest args)
  "Ask a yes/no question requiring full word answer."
  (when format-string
    (apply #'format t format-string args)
    (princ " (yes/no) "))
  (let ((response (string-downcase (read-line))))
    (cond
      ((string= response "yes") t)
      ((string= response "no") nil)
      (t (princ "Please answer yes or no: ")
         (yes-or-no-p)))))

;; ---- time macro ----

(defmacro time (form)
  "Measure and report the execution time of FORM."
  (let ((start (gensym)) (result (gensym)) (elapsed (gensym)))
    `(let* ((,start (get-internal-real-time))
            (,result ,form)
            (,elapsed (- (get-internal-real-time) ,start)))
       (format t "~%Evaluation took ~D ms.~%" ,elapsed)
       ,result)))

;; ---- trace / untrace (basic) ----

(defvar *traced-functions* nil
  "Alist of (name . original-fn) for traced functions.")
(defvar *trace-depth* 0)

(defun %trace-indent ()
  (dotimes (i *trace-depth*)
    (princ "  ")))

(defmacro trace (&rest names)
  "Enable tracing for the named functions."
  `(progn
     ,@(mapcar
        (lambda (name)
          `(unless (assoc ',name *traced-functions*)
             (let ((orig (symbol-function ',name)))
               (push (cons ',name orig) *traced-functions*)
               (defun ,name (&rest args)
                 (%trace-indent)
                 (format t "~D: (~A ~{~S ~})~%" *trace-depth* ',name args)
                 (let ((*trace-depth* (1+ *trace-depth*)))
                   (let ((result (apply (cdr (assoc ',name *traced-functions*)) args)))
                     (%trace-indent)
                     (format t "~D: ~A returned ~S~%" *trace-depth* ',name result)
                     result))))))
        names)
     ',names))

(defmacro untrace (&rest names)
  "Disable tracing for the named functions."
  (if (null names)
      ;; Untrace all
      `(progn
         (dolist (entry *traced-functions*)
           ;; Restore original - simplified, just clear the list
           )
         (setq *traced-functions* nil)
         t)
      `(progn
         ,@(mapcar
            (lambda (name)
              `(let ((entry (assoc ',name *traced-functions*)))
                 (when entry
                   (setq *traced-functions*
                         (remove entry *traced-functions*
                                 :test #'eq)))))
            names)
         ',names)))

;; ---- describe / inspect (basic) ----

(defun describe (object &optional stream)
  "Print a description of OBJECT."
  (let ((type (type-of object)))
    (format t "~%~S~%  [~A]~%" object type)
    (cond
      ((consp object)
       (format t "  Length: ~D~%" (length object)))
      ((stringp object)
       (format t "  Length: ~D~%" (length object)))
      ((vectorp object)
       (format t "  Length: ~D~%" (length object)))
      ((hash-table-p object)
       (format t "  Count: ~D~%" (hash-table-count object)))
      ((numberp object)
       (if (integerp object)
           (format t "  Integer value: ~D~%  Hex: #x~X~%  Binary: #b~B~%" object object object)
           (format t "  Float value: ~F~%" object)))
      ((symbolp object)
       (format t "  Name: ~A~%" (symbol-name object))
       (when (boundp object)
         (format t "  Value: ~S~%" (symbol-value object)))
       (when (fboundp object)
         (format t "  Function: ~S~%" (symbol-function object)))))
    (values)))

(defun inspect (object)
  "Interactively inspect OBJECT (simplified: same as describe)."
  (describe object))

;; ---- documentation (stub) ----

(defvar *documentation-table* (make-hash-table))

(defun documentation (name doc-type)
  "Retrieve documentation string."
  (gethash (list name doc-type) *documentation-table*))

;; ---- with-open-file ----

(defmacro with-open-file (var-spec-and-opts &body body)
  "Open a file, bind VAR (= (car var-spec-and-opts)) to it for BODY,
then close.  VAR-SPEC-AND-OPTS is (var filespec [direction] ...)."
  (let* ((var      (car   var-spec-and-opts))
         (filespec (cadr  var-spec-and-opts))
         (options  (cddr  var-spec-and-opts)))
    `(let ((,var (open ,filespec ,@options)))
       (unwind-protect
           (progn ,@body)
         (when ,var (close ,var))))))

;; ---- with-output-to-string / with-input-from-string ----

(defmacro with-output-to-string (var-and-opts &body body)
  "Bind STREAM-VAR to a fresh string output stream, evaluate BODY,
then return the accumulated string.  VAR-AND-OPTS is (var) or (var
string-init).  Usage: (with-output-to-string (s) (format s ...))."
  (let* ((stream-var (if (consp var-and-opts) (car var-and-opts) var-and-opts))
         (g (gensym)))
    `(let ((,g (make-string-output-stream)))
       (let ((,stream-var ,g))
         ,@body
         (get-output-stream-string ,g)))))

(defmacro with-input-from-string (var-and-opts &body body)
  "Bind STREAM-VAR to a string input stream over STRING.
VAR-AND-OPTS is (var string)."
  (let ((stream-var (car var-and-opts))
        (str-expr (cadr var-and-opts)))
    `(let ((,stream-var (make-string-input-stream ,str-expr)))
       ,@body)))

;; ---- destructuring-bind (basic) ----

(defmacro destructuring-bind (lambda-list expression &body body)
  "Bind variables in LAMBDA-LIST to parts of EXPRESSION."
  (let ((temp (gensym)))
    `(let ((,temp ,expression))
       ,(%expand-destructuring lambda-list temp body))))

(defun %expand-destructuring (pattern expr body)
  "Generate nested LET bindings for destructuring."
  (cond
    ((null pattern)
     `(progn ,@body))
    ((symbolp pattern)
     ;; &rest or simple symbol
     `(let ((,pattern ,expr))
        ,@body))
    ((consp pattern)
     (let ((first (car pattern))
           (rest (cdr pattern)))
       (cond
         ;; &rest / &body
         ((and (symbolp first)
               (or (string= (symbol-name first) "&REST")
                   (string= (symbol-name first) "&BODY")))
          (let ((rest-var (car rest)))
            `(let ((,rest-var ,expr))
               ,@body)))
         ;; &optional
         ((and (symbolp first)
               (string= (symbol-name first) "&OPTIONAL"))
          (%expand-destructuring-optional rest expr body))
         ;; Normal destructuring
         (t
          (let ((car-temp (gensym))
                (cdr-temp (gensym)))
            `(let ((,car-temp (car ,expr))
                   (,cdr-temp (cdr ,expr)))
               ,(%expand-destructuring first car-temp
                  (list (%expand-destructuring rest cdr-temp body)))))))))))

(defun %expand-destructuring-optional (params expr body)
  "Handle &optional in destructuring."
  (if (null params)
      `(progn ,@body)
      (let* ((param (car params))
             (name (if (listp param) (car param) param))
             (default (if (listp param) (cadr param) nil))
             (rest-params (cdr params))
             (temp (gensym)))
        `(let ((,name (if (consp ,expr) (car ,expr) ,default))
               (,temp (if (consp ,expr) (cdr ,expr) nil)))
           ,(%expand-destructuring-optional rest-params temp body)))))

;; ---- boundp / fboundp are provided by C builtins ----

;; ---- multiple-value-bind (if not a special form in eval) ----
;; multiple-value-bind is listed as a symbol but not in eval dispatch.
;; Implement as a macro:

(defmacro multiple-value-bind (vars values-form &body body)
  "Bind VARS to the multiple values returned by VALUES-FORM."
  (let ((vals-list (gensym)))
    `(let ((,vals-list (multiple-value-list ,values-form)))
       (let ,(let ((bindings nil) (i 0))
               (dolist (v vars (nreverse bindings))
                 (push `(,v (nth ,i ,vals-list)) bindings)
                 (incf i)))
         ,@body))))

;; ---- multiple-value-prog1 ----

(defmacro multiple-value-prog1 (first-form &body body)
  "Evaluate FIRST-FORM saving all values, evaluate BODY, return saved values."
  (let ((vals (gensym)))
    `(let ((,vals (multiple-value-list ,first-form)))
       ,@body
       (values-list ,vals))))

;; ---- nth-value ----
;; Already defined above.

;; ---- psetq / psetf ----

(defmacro psetq (&rest pairs)
  "Parallel setq: evaluate all values, then assign."
  (let ((temps nil) (vars nil) (vals nil))
    (do ((p pairs (cddr p)))
        ((null p))
      (let ((g (gensym)))
        (push g temps)
        (push (car p) vars)
        (push (cadr p) vals)))
    (setq temps (nreverse temps))
    (setq vars (nreverse vars))
    (setq vals (nreverse vals))
    `(let ,(mapcar #'list temps vals)
       ,@(mapcar (lambda (v t_) `(setq ,v ,t_)) vars temps)
       nil)))

(defmacro psetf (&rest pairs)
  "Parallel setf: evaluate all values, then assign."
  (let ((temps nil) (places nil) (vals nil))
    (do ((p pairs (cddr p)))
        ((null p))
      (let ((g (gensym)))
        (push g temps)
        (push (car p) places)
        (push (cadr p) vals)))
    (setq temps (nreverse temps))
    (setq places (nreverse places))
    (setq vals (nreverse vals))
    `(let ,(mapcar #'list temps vals)
       ,@(mapcar (lambda (p t_) `(setf ,p ,t_)) places temps)
       nil)))

;; ---- unless / when are special forms, but provide CL-standard behavior ----
;; (already handled in C)

;; ---- prog1 / prog2 already defined above ----

;; ---- and / or are special forms ----

;; ---- notany / notevery ----

(defun notany (predicate sequence)
  "True if PREDICATE is false for all elements."
  (not (some predicate sequence)))

(defun notevery (predicate sequence)
  "True if PREDICATE is false for at least one element."
  (not (every predicate sequence)))

;; ---- rassoc ----

(defun rassoc (item alist &key (test #'eql))
  "Return the first pair in ALIST whose CDR matches ITEM."
  (dolist (pair alist nil)
    (when (and (consp pair) (funcall test item (cdr pair)))
      (return pair))))

;; ---- assoc-if / rassoc-if ----

(defun assoc-if (predicate alist)
  "Return the first pair in ALIST whose CAR satisfies PREDICATE."
  (dolist (pair alist nil)
    (when (and (consp pair) (funcall predicate (car pair)))
      (return pair))))

(defun rassoc-if (predicate alist)
  "Return the first pair in ALIST whose CDR satisfies PREDICATE."
  (dolist (pair alist nil)
    (when (and (consp pair) (funcall predicate (cdr pair)))
      (return pair))))

;; ---- remove-duplicates ----

(defun remove-duplicates (sequence &key (test #'eql) (key #'identity) from-end)
  "Return SEQUENCE with duplicate elements removed."
  (let ((result nil))
    (dolist (x sequence (nreverse result))
      (unless (member (funcall key x) (mapcar key result) :test test)
        (push x result)))))

;; ---- stable-sort (alias for sort) ----

(defun stable-sort (sequence predicate &key (key #'identity))
  "Sort SEQUENCE stably by PREDICATE."
  (sort (copy-list sequence)
        (lambda (a b) (funcall predicate (funcall key a) (funcall key b)))))

;; ---- map ----

(defun map (result-type function sequence &rest more-sequences)
  "Map FUNCTION over SEQUENCE(s), returning a sequence of RESULT-TYPE."
  (let ((result (apply #'mapcar function sequence more-sequences)))
    (if (null result-type)
        nil
        (cond
          ((eq result-type 'list) result)
          ((eq result-type 'vector) (coerce result 'vector))
          ((eq result-type 'string) (coerce result 'string))
          (t result)))))

;; ---- reduce with keywords ----
;; The builtin reduce is minimal; provide a richer version:
;; Actually, the builtin takes keyword args. Override with full version:

;; Keep the builtin reduce; it works for basic cases.

;; ---- fill ----

(defun fill (sequence item &key (start 0) end)
  "Fill SEQUENCE with ITEM."
  (let ((actual-end (or end (length sequence))))
    (dotimes (i (- actual-end start) sequence)
      (setf (nth (+ start i) sequence) item))))

;; ---- concatenate (already a builtin for strings, extend) ----
;; The C builtin 'concatenate' does string concatenation.
;; We keep it as is.

;; ---- coerce is a builtin ----
;; typep is a builtin in lsclos.c ----

;; ---- nsubst / nsubst-if ----

(defun nsubst (new old tree &key (test #'eql))
  "Destructively substitute NEW for OLD in TREE."
  (cond
    ((funcall test old tree) new)
    ((consp tree)
     (rplaca tree (nsubst new old (car tree) :test test))
     (rplacd tree (nsubst new old (cdr tree) :test test))
     tree)
    (t tree)))

;; ---- number formatting ----

(defun integer-to-string (n &optional (radix 10))
  "Convert integer N to a string in given RADIX."
  (cond
    ((zerop n) "0")
    (t (let ((result nil)
             (neg (minusp n))
             (val (abs n)))
         (block nil
           (tagbody
            its-loop
            (when (zerop val) (return nil))
            (let ((digit (mod val radix)))
              (push (digit-char digit radix) result))
            (setq val (truncate (/ val radix)))
            (go its-loop)))
         (when neg (push #\- result))
         (coerce result 'string)))))

;; ---- loop-finish ----
;; For use inside LOOP:
;; (loop-finish) is typically a macro that does (go %loop-end)
(defmacro loop-finish ()
  "Terminate a LOOP iteration early."
  `(go %loop-end))

;; ---- macroexpand / macroexpand-1 ----
;; These might already be provided by the C runtime.
;; Provide stubs if not:

;; ---- symbol-plist (stub) ----

(defvar *symbol-plists* (make-hash-table))

(defun symbol-plist (sym)
  "Return the property list of SYM."
  (gethash sym *symbol-plists*))

(defun get (sym indicator &optional default)
  "Get property INDICATOR from SYM's plist."
  (let ((plist (symbol-plist sym)))
    (do ((p plist (cddr p)))
        ((null p) default)
      (when (eq (car p) indicator)
        (return (cadr p))))))

(defun %put (sym indicator value)
  "Set property INDICATOR on SYM's plist."
  (let ((plist (symbol-plist sym)))
    (do ((p plist (cddr p)))
        ((null p)
         ;; Not found, add it
         (puthash sym *symbol-plists*
                  (list* indicator value plist))
         value)
      (when (eq (car p) indicator)
        (rplaca (cdr p) value)
        (return value)))))

;; (setf (get sym indicator) value) support
(%register-setf-expander 'get
  (lambda (place-args new-val)
    `(%put ,(car place-args) ,(cadr place-args) ,new-val)))

(defun remprop (sym indicator)
  "Remove property INDICATOR from SYM's plist."
  (let ((plist (symbol-plist sym)))
    (cond
      ((null plist) nil)
      ((eq (car plist) indicator)
       (puthash sym *symbol-plists* (cddr plist))
       t)
      (t (do ((p plist (cddr p)))
             ((null (cddr p)) nil)
           (when (eq (caddr p) indicator)
             (rplacd (cdr p) (cddddr p))
             (return t)))))))

;; ---- progv (simplified) ----

(defmacro progv (vars vals &body body)
  "Bind dynamic variables VARS to VALS during BODY (simplified)."
  (let ((old-vals (gensym)) (var-list (gensym)) (val-list (gensym)))
    `(let ((,var-list ,vars)
           (,val-list ,vals)
           (,old-vals nil))
       ;; Save and bind
       (dolist (v ,var-list)
         (push (cons v (if (boundp v) (symbol-value v) '%unbound%)) ,old-vals)
         (eval `(setq ,v ,(pop ,val-list))))
       (unwind-protect
           (progn ,@body)
         ;; Restore
         (dolist (entry ,old-vals)
           (if (eq (cdr entry) '%unbound%)
               nil  ;; Can't truly unbind, leave it
               (eval `(setq ,(car entry) ,(cdr entry)))))))))

;; ---- define-condition (simplified) ----

(defmacro define-condition (name parent-types &rest options)
  "Define a condition type (simplified -- uses defclass under the hood)."
  (let ((slots (cdr (assoc :report options)))
        (slot-defs (let ((sd (assoc :slots options)))
                     (if sd (cdr sd) nil))))
    ;; Very simplified: just create a class
    `(defclass ,name ,(or parent-types '(condition))
       ,@(when slot-defs `(,slot-defs)))))

;; ---- mapcar with multiple lists (wrapper) ----
;; The C builtin only does single list. Provide multi-list version:

(defun %mapcar-multi (fn &rest lists)
  "Apply FN to parallel elements from multiple LISTS."
  (if (some #'null lists)
      nil
      (cons (apply fn (mapcar #'car lists))
            (apply #'%mapcar-multi fn (mapcar #'cdr lists)))))

;; ---- format helpers ----
;; format is a builtin but provide some CL conveniences:

(defun format-to-string (fmt &rest args)
  "Like FORMAT but always returns a string."
  (apply #'format nil fmt args))

;; ---- handler-case for specific common patterns ----

(defmacro with-simple-restart ((restart-name format-string &rest args) &body body)
  "Establish a simple restart around BODY."
  `(restart-case (progn ,@body)
     (,restart-name ()
       :report (lambda (s) (format s ,format-string ,@args))
       (values nil t))))

;; ---- defpackage (stub) ----

(defmacro defpackage (name &rest options)
  "Define a package (simplified stub)."
  `(or (find-package ,(string name))
       (error "defpackage: package creation not fully supported")))

(defmacro in-package (name)
  "Set the current package (stub)."
  `(or (find-package ,(string name))
       (error "in-package: package not found")))

;; ---- export / use-package stubs ----
;; export is a builtin

;; ---- provide / require (simplified) ----

(defvar *modules* nil)

(defun provide (module-name)
  "Record that MODULE-NAME has been provided."
  (pushnew (string module-name) *modules* :test #'string=)
  t)

(defun require (module-name &optional pathname)
  "Load MODULE-NAME if not already provided."
  (unless (member (string module-name) *modules* :test #'string=)
    (if pathname
        (load pathname)
        (load (string-downcase (string module-name))))))

;; ---- char-name / name-char (basic) ----

(defun char-name (c)
  "Return the name of character C."
  (case c
    (#\Space "Space")
    (#\Newline "Newline")
    (#\Tab "Tab")
    (otherwise nil)))

(defun name-char (name)
  "Return the character with the given NAME."
  (cond
    ((string-equal name "Space") #\Space)
    ((string-equal name "Newline") #\Newline)
    ((string-equal name "Tab") #\Tab)
    (t nil)))

;; ---- whitespacep ----

(defun whitespacep (c)
  "True if C is a whitespace character."
  (member c '(#\Space #\Newline #\Tab #\Return)))

;; ---- alphanumericp ----

(defun alphanumericp (c)
  "True if C is alphanumeric."
  (or (alpha-char-p c) (digit-char-p c)))

;; ---- both-case-p ----

(defun both-case-p (c)
  "True if C has both upper and lower case variants."
  (alpha-char-p c))

;; ---- graphic-char-p ----

(defun graphic-char-p (c)
  "True if C is a graphic (printable) character."
  (let ((code (char-code c)))
    (and (>= code 32) (<= code 126))))

;; ---- standard-char-p ----

(defun standard-char-p (c)
  "True if C is a standard character."
  (let ((code (char-code c)))
    (or (and (>= code 32) (<= code 126))
        (= code 10))))  ;; newline

;; ---- char-equal (case-insensitive) ----

(defun char-equal (c1 c2)
  "Case-insensitive character comparison."
  (char= (char-upcase c1) (char-upcase c2)))

(defun char-not-equal (c1 c2)
  "Case-insensitive char/=."
  (not (char-equal c1 c2)))

(defun char-lessp (c1 c2)
  "Case-insensitive char<."
  (char< (char-upcase c1) (char-upcase c2)))

(defun char-greaterp (c1 c2)
  "Case-insensitive char>."
  (char> (char-upcase c1) (char-upcase c2)))

(defun char-not-greaterp (c1 c2)
  "Case-insensitive char<=."
  (char<= (char-upcase c1) (char-upcase c2)))

(defun char-not-lessp (c1 c2)
  "Case-insensitive char>=."
  (char>= (char-upcase c1) (char-upcase c2)))

;; ---- string-capitalize is provided as a C builtin ----

;; ---- parse-integer (enhanced) ----
;; The C version is minimal. Override with better version.

;; ---- delete / delete-if / delete-if-not (destructive remove) ----

(defun delete (item sequence &key (test #'eql) (key #'identity))
  "Destructively remove elements matching ITEM."
  (remove item sequence :test test :key key))

(defun delete-if (predicate sequence &key (key #'identity))
  "Destructively remove elements satisfying PREDICATE."
  (remove-if predicate sequence))

(defun delete-if-not (predicate sequence &key (key #'identity))
  "Destructively remove elements not satisfying PREDICATE."
  (remove-if-not predicate sequence))

;; ---- subsetp ----

(defun subsetp (list1 list2 &key (test #'eql))
  "True if every element of LIST1 is in LIST2."
  (every (lambda (x) (member x list2)) list1))

;; ---- list-length (handles circular lists) ----

(defun list-length (list)
  "Return the length of LIST, or NIL if circular."
  (let ((slow list) (fast list) (n 0))
    (block nil
      (tagbody
       ll-loop
       (when (null fast) (return n))
       (when (null (cdr fast)) (return (1+ n)))
       (when (eq (cdr fast) slow) (return nil))
       (setq slow (cdr slow))
       (setq fast (cddr fast))
       (incf n 2)
       (go ll-loop)))))

;; ---- revappend / nreconc ----

(defun revappend (list tail)
  "Append TAIL to the reverse of LIST."
  (if (null list)
      tail
      (revappend (cdr list) (cons (car list) tail))))

(defun nreconc (list tail)
  "Destructive version of REVAPPEND."
  (nconc (nreverse list) tail))

;; ---- make-list already defined above ----

;; ---- last with N argument ----
;; The builtin last returns last cons. This is correct for CL.

;; ---- getf / remf (property list operations) ----

(defun getf (plist indicator &optional default)
  "Get the value for INDICATOR from property list PLIST."
  (do ((p plist (cddr p)))
      ((null p) default)
    (when (eq (car p) indicator)
      (return (cadr p)))))

(defmacro remf (place indicator)
  "Remove INDICATOR and its value from the property list at PLACE."
  (let ((plist (gensym)) (ind (gensym)))
    `(let ((,plist ,place) (,ind ,indicator))
       (cond
         ((null ,plist) nil)
         ((eq (car ,plist) ,ind)
          (setf ,place (cddr ,plist))
          t)
         (t (do ((p ,plist (cddr p)))
                ((null (cddr p)) nil)
              (when (eq (caddr p) ,ind)
                (rplacd (cdr p) (cddddr p))
                (return t))))))))

;; ---- multiple-value-setq ----

(defmacro multiple-value-setq (vars form)
  "Set VARS to the multiple values of FORM."
  (let ((vals (gensym)))
    `(let ((,vals (multiple-value-list ,form)))
       ,@(let ((sets nil) (i 0))
           (dolist (v vars (nreverse sets))
             (push `(setq ,v (nth ,i ,vals)) sets)
             (incf i)))
       ,(car vars))))

;; ---- with-accessors / with-slots (CLOS convenience) ----

(defmacro with-slots (slot-names instance &body body)
  "Bind variables to slots of INSTANCE."
  (let ((inst (gensym)))
    `(let ((,inst ,instance))
       (let ,(mapcar (lambda (s)
                       (let ((name (if (listp s) (car s) s))
                             (slot (if (listp s) (cadr s) s)))
                         `(,name (slot-value ,inst ',slot))))
                     slot-names)
         ,@body))))

(defmacro with-accessors (accessor-bindings instance &body body)
  "Bind local variables to ACCESSOR calls on INSTANCE.
Each binding has the form (var-name accessor-fn)."
  (let ((inst (gensym)))
    `(let ((,inst ,instance))
       (let ,(mapcar (lambda (b) `(,(car b) (,(cadr b) ,inst)))
                     accessor-bindings)
         ,@body))))

;; ---- ccase / cerror-style continuable case ----

(defmacro ccase (keyform &rest clauses)
  "Like ECASE, but offers a (continuable) restart on failure.
Currently equivalent to ECASE -- there is no interactive restart
prompting yet."
  `(ecase ,keyform ,@clauses))

;; ---- Interactive helpers ----

(defun prompt (prompt-string)
  "Print PROMPT-STRING and read a line from standard input."
  (princ prompt-string)
  (force-output)
  (read-line))

(defun prompt-for (prompt-string)
  "Print PROMPT, then READ a Lisp form from input."
  (princ prompt-string)
  (force-output)
  (read))

(defmacro until (test &body body)
  "Execute BODY repeatedly until TEST returns true."
  `(loop until ,test do (progn ,@body)))

(defmacro while (test &body body)
  "Execute BODY repeatedly while TEST returns true."
  `(loop while ,test do (progn ,@body)))

(defmacro repeat-until (count &body body)
  "Execute BODY COUNT times."
  (let ((g (gensym)))
    `(dotimes (,g ,count) ,@body)))

;; ---- simple-condition helpers ----

(defun warn (format-string &rest args)
  "Print a warning message."
  (format *error-output* "~&WARNING: ")
  (apply #'format *error-output* format-string args)
  (terpri *error-output*)
  nil)

;; *error-output* might not exist; define it:
(defvar *error-output* t)
(defvar *standard-output* t)
(defvar *standard-input* t)
(defvar *debug-io* t)
(defvar *query-io* t)

;; ---- string-not-equal etc. ----

(defun string-not-equal (s1 s2)
  "Case-insensitive string/=."
  (not (string-equal s1 s2)))

(defun string-not-lessp (s1 s2)
  "Case-insensitive string>=."
  (not (string-lessp s1 s2)))

(defun string-not-greaterp (s1 s2)
  "Case-insensitive string<=."
  (not (string-greaterp s1 s2)))

;; ---- collect-to-string ----

(defun collect-to-string (fn)
  "Call FN and capture output as a string (simplified stub)."
  ;; Would need stream capture support from the runtime
  "")

;; ---- array-dimensions / array-total-size ----

(defun array-dimensions (array)
  "Return the dimensions of ARRAY."
  (list (length array)))

(defun array-total-size (array)
  "Return the total number of elements in ARRAY."
  (length array))

(defun array-rank (array)
  "Return the rank (number of dimensions) of ARRAY."
  1)  ;; We only support 1D arrays

;; ---- string-starts-with / string-ends-with (extension) ----

(defun string-starts-with (string prefix)
  "True if STRING starts with PREFIX."
  (let ((slen (length string))
        (plen (length prefix)))
    (and (>= slen plen)
         (string= (subseq string 0 plen) prefix))))

(defun string-ends-with (string suffix)
  "True if STRING ends with SUFFIX."
  (let ((slen (length string))
        (suflen (length suffix)))
    (and (>= slen suflen)
         (string= (subseq string (- slen suflen)) suffix))))

;; ---- string-contains ----

(defun string-contains (string substring)
  "True if STRING contains SUBSTRING. Returns position or NIL."
  (let ((slen (length string))
        (sublen (length substring)))
    (if (= sublen 0) 0
        (dotimes (i (1+ (- slen sublen)) nil)
          (when (string= (subseq string i (+ i sublen)) substring)
            (return i))))))

;; ---- nstring-upcase / nstring-downcase ----

(defun nstring-upcase (string)
  "Destructively upcase STRING."
  (string-upcase string))

(defun nstring-downcase (string)
  "Destructively downcase STRING."
  (string-downcase string))

;; ---- multiple-value-call ----
;; This is listed as a special form symbol but not implemented.
;; Provide a macro version:

(defmacro multiple-value-call (function &rest forms)
  "Call FUNCTION with all values from all FORMS."
  (if (null forms)
      `(funcall ,function)
      (let ((args (gensym)))
        `(let ((,args nil))
           ,@(mapcar (lambda (f)
                       `(setq ,args (append ,args (multiple-value-list ,f))))
                     forms)
           (apply ,function ,args)))))

;; ---- every / some with multiple sequences ----
;; Builtins handle single list. That's fine for now.

;; ---- map-into (simplified) ----

(defun map-into (result-sequence function &rest sequences)
  "Map FUNCTION into RESULT-SEQUENCE."
  (let ((len (length result-sequence)))
    (dotimes (i len result-sequence)
      (setf (nth i result-sequence)
            (apply function
                   (mapcar (lambda (s) (nth i s)) sequences))))))

;; ---- type-predicates ----
;; Most are builtins (numberp, integerp, stringp, etc.)
;; Add a few missing ones:

(defun characterp (x)
  "True if X is a character."
  ;; This is actually a C builtin, but alias just in case
  (eq (type-of x) 'character))

(defun simple-string-p (x)
  "True if X is a simple string."
  (stringp x))

(defun simple-vector-p (x)
  "True if X is a simple vector."
  (vectorp x))

(defun arrayp (x)
  "True if X is an array (or vector)."
  (vectorp x))

(defun bit-vector-p (x)
  "True if X is a bit vector (stub)."
  nil)

(defun compiled-function-p (x)
  "True if X is a compiled function."
  (functionp x))

(defun complexp (x)
  "True if X is a complex number (stub)."
  nil)

(defun rationalp (x)
  "True if X is rational."
  (integerp x))

(defun realp (x)
  "True if X is a real number."
  (numberp x))

;; ---- handler macros already defined above ----

;; ---- define-compiler-macro (stub) ----

(defmacro define-compiler-macro (name lambda-list &body body)
  "Define a compiler macro (no-op in interpreter)."
  `',name)

;; ---- declaim (simplified) ----

(defmacro declaim (&rest declarations)
  "Process top-level declarations (simplified -- mostly no-op)."
  nil)

(defmacro proclaim (declaration)
  "Process a declaration (simplified -- no-op)."
  nil)

;; ---- gentemp ----

(defvar *gentemp-counter* 0)

(defun gentemp (&optional (prefix "T") (package nil))
  "Generate and intern a unique symbol."
  (incf *gentemp-counter*)
  (intern (concatenate prefix (integer-to-string *gentemp-counter*))))

;; ---- lambda macro (for CL conformance) ----
;; lambda is a special form, but (lambda ...) as a macro should work.
;; It's already handled in lseval.c.

;; ---- constantly / identity already defined ----

;; ---- make-symbol ----
;; gensym is a builtin. make-symbol might not be:

(defun make-symbol (name)
  "Create an uninterned symbol with the given NAME."
  (gensym))  ;; Simplified -- always uninterned

;; ---- copy-symbol (simplified) ----

(defun copy-symbol (symbol &optional copy-props)
  "Create a copy of SYMBOL."
  (let ((new (make-symbol (symbol-name symbol))))
    new))

;; ---- special-operator-p ----

(defvar *special-operators*
  '(quote if progn setq let let* lambda function
    defun defmacro defvar defparameter defconstant
    block return-from catch throw unwind-protect
    tagbody go and or when unless cond
    eval-when locally the declare flet labels
    multiple-value-bind multiple-value-call
    multiple-value-prog1))

(defun special-operator-p (symbol)
  "True if SYMBOL names a special operator."
  (member symbol *special-operators*))

;; ---- macro-function ----

(defun macro-function (symbol)
  "Return the macro function of SYMBOL, or NIL."
  (if (and (symbolp symbol)
           (fboundp symbol))
      (let ((fn (symbol-function symbol)))
        ;; Check if it's a macro
        fn)
      nil))

;; ---- Startup message ----

(format t "~%; Litesrpent standard library loaded.~%")
