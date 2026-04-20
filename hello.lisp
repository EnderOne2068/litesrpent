;;; hello.lisp -- Hello World and basic demonstrations

(format t "Hello from Litesrpent!~%")
(format t "Version: ~A~%" "0.1.0")

;; Arithmetic
(format t "~%--- Arithmetic ---~%")
(format t "2 + 3 = ~D~%" (+ 2 3))
(format t "10 / 3 = ~F~%" (/ 10.0 3.0))
(format t "2^10 = ~D~%" (expt 2 10))
(format t "sqrt(144) = ~D~%" (truncate (sqrt 144)))

;; Lists
(format t "~%--- Lists ---~%")
(defvar *fruits* '(apple banana cherry date elderberry))
(format t "Fruits: ~A~%" *fruits*)
(format t "First: ~A~%" (first *fruits*))
(format t "Length: ~D~%" (length *fruits*))
(format t "Reversed: ~A~%" (reverse *fruits*))

;; Functions
(format t "~%--- Functions ---~%")
(defun square (x) (* x x))
(defun cube (x) (* x x x))
(format t "square(7) = ~D~%" (square 7))
(format t "cube(3) = ~D~%" (cube 3))
(format t "mapcar square (1 2 3 4 5) = ~A~%" (mapcar #'square '(1 2 3 4 5)))

;; Higher-order functions
(format t "~%--- Higher-order ---~%")
(format t "sum 1..10 = ~D~%" (reduce #'+ '(1 2 3 4 5 6 7 8 9 10)))
(format t "evens: ~A~%" (remove-if-not #'evenp '(1 2 3 4 5 6 7 8 9 10)))

;; Closures
(format t "~%--- Closures ---~%")
(defun make-counter (&optional (start 0))
  (let ((n start))
    (lambda () (setq n (1+ n)) n)))
(defvar *counter* (make-counter))
(format t "counter: ~D ~D ~D~%" (funcall *counter*) (funcall *counter*) (funcall *counter*))

;; Recursive Fibonacci
(format t "~%--- Fibonacci ---~%")
(defun fib (n)
  (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
(format t "fib(10) = ~D~%" (fib 10))
(format t "fib(20) = ~D~%" (fib 20))

(format t "~%Done!~%")
