/*
 * Copyright by Denys Khanzhiyev aka xdenser
 *
 * See license text in LICENSE file
 */
#include "./fb-bindings-connection.h" 
#include "./fb-bindings-fbresult.h"
#include "./fb-bindings-statement.h"

void
  Connection::Initialize (v8::Handle<v8::Object> target)
  {
    // HandleScope scope;
    
    Local<FunctionTemplate> t =  Nan::New<FunctionTemplate>(Connection::New);
    
    t->InstanceTemplate()->SetInternalFieldCount(1);
    t->SetClassName(Nan::New("Connection").ToLocalChecked());
    t->Inherit(Nan::New(FBEventEmitter::constructor_template));
    // Methods 

    Nan::SetPrototypeMethod(t, "connectSync", ConnectSync);
    Nan::SetPrototypeMethod(t, "connect", Connect);
    Nan::SetPrototypeMethod(t, "querySync", QuerySync);
    Nan::SetPrototypeMethod(t, "query", Query);
    Nan::SetPrototypeMethod(t, "disconnect", Disconnect);
    Nan::SetPrototypeMethod(t, "commitSync", CommitSync);
    Nan::SetPrototypeMethod(t, "commit", Commit);
    Nan::SetPrototypeMethod(t, "rollbackSync", RollbackSync);
    Nan::SetPrototypeMethod(t, "rollback", Rollback);
    Nan::SetPrototypeMethod(t, "addFBevent", addEvent);
    Nan::SetPrototypeMethod(t, "deleteFBevent", deleteEvent);
    Nan::SetPrototypeMethod(t, "prepareSync", PrepareSync);
    Nan::SetPrototypeMethod(t, "newBlobSync", NewBlobSync);
    Nan::SetPrototypeMethod(t, "startTransactionSync", StartSync);
    Nan::SetPrototypeMethod(t, "startTransaction", Start);
    
    // Properties
    Local<v8::ObjectTemplate> instance_t = t->InstanceTemplate();
    Nan::SetAccessor(instance_t, Nan::New("connected").ToLocalChecked(), ConnectedGetter);
    Nan::SetAccessor(instance_t, Nan::New("inTransaction").ToLocalChecked(), InTransactionGetter);
    Nan::SetAccessor(instance_t, Nan::New("inAsyncCall").ToLocalChecked(),InAsyncGetter);
   
    
    target->Set(Nan::New("Connection").ToLocalChecked(), t->GetFunction());
  }
  
bool Connection::Connect (const char* Database,const char* User,const char* Password,const char* Role)
  {
    if (db) return false;
    short i = 0, len;
    char dpb[256];
    char *lc_type = const_cast<char *>("UTF8");
    
    dpb[i++] = isc_dpb_version1;
    
    dpb[i++] = isc_dpb_user_name;
    len = (short) strlen (User);
	if (len > MAX_USERNAME_LENGTH) {
		len = MAX_USERNAME_LENGTH;
	}
    dpb[i++] = (char) len;
    strncpy(&(dpb[i]), User, len);
    i += len;
    
    dpb[i++] = isc_dpb_password;
    len = (short) strlen (Password);
	if (len > MAX_PASSWORD_LENGTH) {
		len = MAX_PASSWORD_LENGTH;
	}
    dpb[i++] = (char) len;
    strncpy(&(dpb[i]), Password, len);
    i += len;

    dpb[i++] = isc_dpb_sql_role_name;
    len = (short) strlen (Role);
	if (len > MAX_ROLENAME_LENGTH) {
		len = MAX_ROLENAME_LENGTH;
	}
    dpb[i++] = (char) len;
    strncpy(&(dpb[i]), Role, len);
    i += len;
    
    dpb[i++] = isc_dpb_lc_ctype;
    len = (short) strlen (lc_type);
    dpb[i++] = (char) len;
    strncpy(&(dpb[i]), lc_type, len);
    i += len;
    
    connected = false;
    if(isc_attach_database(status, 0, Database, &(db), i, dpb)) return false;
    connected = true;
    return true;
  } 
  
  
bool Connection::Close(){
    
    if(trans)
     if(!commit_transaction()) {
       return false;
     }
    
    if(fb_events) free(fb_events);
    fb_events = NULL;
 
    connected = false; 
    if (isc_detach_database(status, &db)) {
      db = NULL;
      return false;
    } 
    db = NULL;
    return true;
    
  }
  
bool Connection::process_statement(XSQLDA **sqldap, char *query, isc_stmt_handle *stmtp, int *statement_type)
  {
     XSQLDA          *sqlda;
  //   XSQLVAR         *var;
     static char     stmt_info[] = { isc_info_sql_stmt_type };
     char            info_buffer[20];
     short           l, num_cols;// i, length, alignment, type, offset;
    // int             statement_type;
     int 	     sqlda_size;	
     
     
     sqlda = *sqldap;
     
     if(!sqlda)
     {
          sqlda_size = XSQLDA_LENGTH (20);
          sqlda = (XSQLDA *) malloc(sqlda_size);
          memset(sqlda,0,sqlda_size);
          sqlda->sqln = 20;
          sqlda->version = 1;
          *sqldap = sqlda;
     }      

     /* Allocate a statement */
     if(!*stmtp) {
      if (isc_dsql_allocate_statement(status, &db, stmtp)) return false;
     } 
     
     // Start Default Transaction If None Active
     if(!trans) 
      {
        if (isc_start_transaction(status, &trans, 1, &db, 0, NULL)) return false;
      }
      
     // Prepare Statement
     if (isc_dsql_prepare(status, &trans, stmtp, 0, query, SQL_DIALECT_V6, sqlda)) {  return false; }

     // Get sql info
     if (!isc_dsql_sql_info(status, stmtp, sizeof (stmt_info), stmt_info, 
         sizeof (info_buffer), info_buffer))
     {
         l = (short) isc_vax_integer((char *) info_buffer + 1, 2);
         *statement_type = isc_vax_integer((char *) info_buffer + 3, l);
     }
     
     // It is statement w\o resultset
     if (!sqlda->sqld)
     {
        free(sqlda);
        *sqldap = NULL;
                   
        if (isc_dsql_execute(status, &trans, stmtp, SQL_DIALECT_V6, NULL))
        {
             return false;
        }

        /* Commit DDL statements if that is what sql_info says */

        if (trans && (*statement_type == isc_info_sql_stmt_ddl))
        {
            //printf("it is ddl !");
            if (isc_commit_transaction(status, &trans))
            {
             trans = 0;
             return false;    
            }    
            trans = 0;
        }

        return true;
     }

     /*
      *    Process select statements.
      */

     num_cols = sqlda->sqld;

     /* Need more room. */
     if (sqlda->sqln < num_cols)
     {
        sqlda_size = XSQLDA_LENGTH (num_cols);
        *sqldap = sqlda = (XSQLDA *) realloc( sqlda, sqlda_size);
        memset(sqlda,0,sqlda_size);                                        
        sqlda->sqln = num_cols;
        sqlda->version = 1;

        if (isc_dsql_describe(status, stmtp, SQL_DIALECT_V6, sqlda))
        {
            return false;
        }

        num_cols = sqlda->sqld;
     }

     /*
      *     Set up SQLDA.
      */
    if(!FBResult::prepare_sqlda(sqlda)) { return false; };
      
     if(*statement_type == isc_info_sql_stmt_select){
    	 if (isc_dsql_execute(status, &trans, stmtp, SQL_DIALECT_V6, NULL))
    	 {
    		 return false;
    	 }
     }
     else {
    	 if (isc_dsql_execute2(status, &trans, stmtp, SQL_DIALECT_V6, NULL, sqlda)){
    		 return false;
    	 }
     }
    	 
     
    return true;

  }

bool Connection::prepare_statement(XSQLDA **insqlda, XSQLDA **outsqlda, char *query, isc_stmt_handle *stmtp)
  {
     XSQLDA          *in_sqlda;
     XSQLDA          *out_sqlda;
     short 	     num_params, num_cols;
     int 	     sqlda_size;
     
     in_sqlda = *insqlda;
     if(!in_sqlda)
     {
    	  sqlda_size = XSQLDA_LENGTH (10);
    	  in_sqlda = (XSQLDA *) malloc(sqlda_size);
          memset(in_sqlda,0,sqlda_size);
          in_sqlda->sqln = 10;
          in_sqlda->version = 1;
          *insqlda = in_sqlda;
     }      

     out_sqlda = *outsqlda;
     if(!out_sqlda)
     {
    	  sqlda_size = XSQLDA_LENGTH (10);
    	  out_sqlda = (XSQLDA *) malloc(sqlda_size);
    	  memset(out_sqlda,0,sqlda_size);
          out_sqlda->sqln = 10;
          out_sqlda->version = 1;
          *outsqlda = out_sqlda;
     }      
     
     
    /* Allocate a statement */
     if(!*stmtp) {
      if (isc_dsql_allocate_statement(status, &db, stmtp)) return false;
     } 
     
     // Start Default Transaction If None Active
     if(!trans) 
     {
      if (isc_start_transaction(status, &trans, 1, &db, 0, NULL)) return false;
     }
     
     // Prepare Statement
     if (isc_dsql_prepare(status, &trans, stmtp, 0, query, SQL_DIALECT_V6, out_sqlda)) return false;
     
     // Describe bind
     if (isc_dsql_describe_bind(status, stmtp, 1, in_sqlda)) return false;
     
     /* Need more room. */
     if (in_sqlda->sqln < in_sqlda->sqld)
     {
        num_params = in_sqlda->sqld;
        
        sqlda_size = XSQLDA_LENGTH (num_params);
        *insqlda = in_sqlda = (XSQLDA *) realloc(in_sqlda, sqlda_size);
        memset(in_sqlda,0,sqlda_size);
        
        in_sqlda->sqln = num_params;
        in_sqlda->version = 1;
        
        // Describe bind again
        if (isc_dsql_describe_bind(status, stmtp, 1, in_sqlda)) return false;

        num_params = in_sqlda->sqld;
     }
     
    /*
     *     Set up SQLDA.
     */
    if(!FBResult::prepare_sqlda(in_sqlda)) return false;
     
     /* Need more room. */
    if (out_sqlda->sqln < out_sqlda->sqld)
     {
        num_cols = out_sqlda->sqld; 
        sqlda_size = XSQLDA_LENGTH (num_cols);
        *outsqlda = out_sqlda = (XSQLDA *) realloc(out_sqlda, sqlda_size);
        memset(out_sqlda,0,sqlda_size);
        
        out_sqlda->sqln = num_cols;
        out_sqlda->version = 1;

        if (isc_dsql_describe(status, stmtp, SQL_DIALECT_V6, out_sqlda))
        {
            return false;
        }

        num_cols = out_sqlda->sqld;
     }
     
    /*
     *     Set up SQLDA.
     */
    if(!FBResult::prepare_sqlda(out_sqlda)) return false;
         
    return true;     
  }  

  
bool Connection::commit_transaction()
  {
    if (isc_commit_transaction(status, &trans))
    {
             trans = 0;
             return false;    
    }    
    trans = 0;
    return true;
  }
    
bool Connection::rollback_transaction()
  {
    if (isc_rollback_transaction(status, &trans))
    {
             trans = 0;
             return false;    
    }    
    trans = 0;
    return true;
  }

bool Connection::start_transaction()
  {
    if(!trans) 
      {
        if (isc_start_transaction(status, &trans, 1, &db, 0, NULL))
        {
          trans = 0;
          ERR_MSG(this,Connection);
          return false;
        }
        return true;  
      }
      
    strncpy(err_message, "Old transaction should be commited or rolled back.", MAX_ERR_MSG_LEN);
    return false;  
  }
  
isc_db_handle Connection::get_DB_Handle()
  {
    return db;
  }
  
isc_tr_handle Connection::get_Def_Tr_Handle()
  {
    return trans;
  }   

void Connection::doref()
{
	this->Ref();
}

void Connection::dounref()
{
	this->Unref();
}

NAN_METHOD(Connection::New)
  {
	 Nan::HandleScope scope;

    Connection *connection = new Connection();
    connection->Wrap(info.This());

    info.GetReturnValue().Set(info.This());
  }

NAN_METHOD(Connection::ConnectSync)
  {

    Nan::HandleScope scope;
    Connection *connection = Nan::ObjectWrap::Unwrap<Connection>(info.This());

    if (info.Length() < 4 || !info[0]->IsString() ||
         !info[1]->IsString() || !info[2]->IsString() ||
         !info[3]->IsString()) {
      return Nan::ThrowError("Expecting 4 string arguments");
    }

    String::Utf8Value Database(info[0]->ToString());
    String::Utf8Value User(info[1]->ToString());
    String::Utf8Value Password(info[2]->ToString());
    String::Utf8Value Role(info[3]->ToString());
    

    bool r = connection->Connect(*Database,*User,*Password,*Role);

    if (!r) {
      return Nan::ThrowError(
            String::Concat(Nan::New("While connecting - ").ToLocalChecked(),ERR_MSG(connection, Connection)));
    }
    
    return;
  }

   
void Connection::EIO_After_Connect(uv_work_t *req)
  {
    Nan::HandleScope scope;
    struct connect_request *conn_req = (struct connect_request *)(req->data);
	delete req;
    Local<Value> argv[1];
    
    if (!conn_req->res) {
       argv[0] = Nan::Error(*Nan::Utf8String(
            String::Concat(Nan::New("While connecting - ").ToLocalChecked(),ERR_MSG(conn_req->conn, Connection))));
    }
    else{
     argv[0] = Nan::Null();
    }
   
    TryCatch try_catch;

    conn_req->callback->Call( 1, argv);

    if (try_catch.HasCaught()) {
        node::FatalException(try_catch);
    }
    
    conn_req->conn->stop_async();
   // conn_req->conn->Unref();
    free(conn_req);
    
  }
  
void Connection::EIO_Connect(uv_work_t *req)
  {
    struct connect_request *conn_req = (struct connect_request *)(req->data);
    conn_req->res = conn_req->conn->Connect(
                     **(conn_req->Database),
                     **(conn_req->User), 
                     **(conn_req->Password),
                     **(conn_req->Role));
    delete conn_req->Database;
    delete conn_req->User;
    delete conn_req->Password;
    delete conn_req->Role;
    
    return ;
   
  }
  
NAN_METHOD(Connection::Connect)
  {
	Nan::HandleScope scope;
    Connection *conn = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    
    
    struct connect_request *conn_req =
         (struct connect_request *)calloc(1, sizeof(struct connect_request));

    if (!conn_req) {
      Nan::LowMemoryNotification();
      return Nan::ThrowError("Could not allocate memory.");
    }
    
    if (info.Length() < 5 || !info[0]->IsString() ||
         !info[1]->IsString() || !info[2]->IsString() ||
         !info[3]->IsString() || !info[4]->IsFunction()) {
      return Nan::ThrowError("Expecting 4 string arguments and 1 Function");
    }
    
    conn_req->conn = conn;
    conn_req->callback =  new Nan::Callback(Local<Function>::Cast(info[4]));
    conn_req->Database = new String::Utf8Value(info[0]->ToString());
    conn_req->User     = new String::Utf8Value(info[1]->ToString());
    conn_req->Password = new String::Utf8Value(info[2]->ToString());
    conn_req->Role     = new String::Utf8Value(info[3]->ToString());
    
    conn->start_async();
    
	uv_work_t* req = new uv_work_t();
    req->data = conn_req;
    uv_queue_work(uv_default_loop(), req, EIO_Connect,  (uv_after_work_cb)EIO_After_Connect);

    
   // uv_ref(uv_default_loop());
   // conn->Ref();
    
    return;
  }

NAN_METHOD(Connection::Disconnect)
  {
    Nan::HandleScope scope;
    Connection *connection = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    //printf("disconnect\n");
    if(!connection->Close()){
      return Nan::ThrowError(
            String::Concat(Nan::New("While closing - ").ToLocalChecked(),ERR_MSG(connection, Connection)));
    }     
   
    return;
  }
  
NAN_GETTER(Connection::ConnectedGetter)
  {
    Nan::HandleScope scope;
    Connection *connection = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    info.GetReturnValue().Set(Nan::New<Boolean>(connection->connected));
  }
  
NAN_GETTER(Connection::InTransactionGetter)
  {
    Nan::HandleScope scope;
    Connection *connection = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    info.GetReturnValue().Set(Nan::New<Boolean>(connection->trans));
  }

NAN_METHOD(Connection::CommitSync)
  {
    Nan::HandleScope scope;
    Connection *connection = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    
    bool r = connection->commit_transaction();

    if (!r) {
      return Nan::ThrowError(
            String::Concat(Nan::New("While commitSync - ").ToLocalChecked(),ERR_MSG(connection, Connection)));
    }
    
    return;
  } 
    
NAN_METHOD(Connection::RollbackSync)
  {
    Nan::HandleScope scope;
    Connection *connection = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    
    bool r = connection->rollback_transaction();

    if (!r) {
      return Nan::ThrowError(
            String::Concat(Nan::New("While rollbackSync - ").ToLocalChecked(),ERR_MSG(connection, Connection)));
    }
    
    return;
  } 
  
NAN_METHOD(Connection::StartSync)
  {
    Nan::HandleScope scope;
    Connection *connection = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    
    bool r = connection->start_transaction();

    if (!r) {
      return Nan::ThrowError(connection->err_message);
    }
    
    return;
  }
  
  
void Connection::EIO_After_TransactionRequest(uv_work_t *req)
  {
  //  uv_unref(uv_default_loop());
    Nan::HandleScope scope;
    struct transaction_request *tr_req = (struct transaction_request *)(req->data);
    delete req;
    Local<Value> argv[1];
    
    if (!tr_req->result) {
       argv[0] = Nan::Error(*Nan::Utf8String(ERR_MSG(tr_req->conn, Connection)));
    }
    else{
     argv[0] = Nan::Null();
    }
   
    TryCatch try_catch;

    tr_req->callback->Call(1, argv);

    if (try_catch.HasCaught()) {
        node::FatalException(try_catch);
    }

    tr_req->conn->stop_async();
    tr_req->conn->Unref();
    free(tr_req);

    
  }
  
void Connection::EIO_TransactionRequest(uv_work_t *req)
  {
    struct transaction_request *tr_req = (struct transaction_request *)(req->data);
    switch(tr_req->type){
       case rCommit:
            tr_req->result = tr_req->conn->commit_transaction();
            break;
       case rRollback:
            tr_req->result = tr_req->conn->rollback_transaction();   
            break;
       case rStart:
            tr_req->result = tr_req->conn->start_transaction();   
    }
    return;
  }

  
NAN_METHOD(Connection::Commit)
  {
    Nan::HandleScope scope;
    Connection *conn = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    
    struct transaction_request *tr_req =
         (struct transaction_request *)calloc(1, sizeof(struct transaction_request));

    if (!tr_req) {
      Nan::LowMemoryNotification();
      return Nan::ThrowError("Could not allocate memory.");
    }
    
    if (info.Length() < 1) {
      return Nan::ThrowError("Expecting Callback Function argument");
    }
    
    tr_req->conn = conn;
    tr_req->callback = new Nan::Callback(Local<Function>::Cast(info[0]));
    tr_req->type = rCommit;
    
    conn->start_async();

    uv_work_t* req = new uv_work_t();
    req->data = tr_req;
    uv_queue_work(uv_default_loop(), req, EIO_TransactionRequest,  (uv_after_work_cb)EIO_After_TransactionRequest);

    
    //uv_ref(uv_default_loop());
    conn->Ref();
    
    return;
  }
  
NAN_METHOD(Connection::Rollback)
  {
    Nan::HandleScope scope;
    Connection *conn = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    
    struct transaction_request *tr_req =
         (struct transaction_request *)calloc(1, sizeof(struct transaction_request));

    if (!tr_req) {
      Nan::LowMemoryNotification();
      return Nan::ThrowError("Could not allocate memory.");
    }
    
    if (info.Length() < 1) {
      return Nan::ThrowError("Expecting Callback Function argument");
    }
    
    tr_req->conn = conn;
    tr_req->callback = new Nan::Callback(Local<Function>::Cast(info[0]));
    tr_req->type = rRollback;
    
    conn->start_async();

	uv_work_t* req = new uv_work_t();
    req->data = tr_req;
    uv_queue_work(uv_default_loop(), req, EIO_TransactionRequest,  (uv_after_work_cb)EIO_After_TransactionRequest);
    
   // uv_ref(uv_default_loop());
    conn->Ref();
    
    return;
  } 
  
NAN_METHOD(Connection::Start)
  {
    Nan::HandleScope scope;
    Connection *conn = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    
    struct transaction_request *tr_req =
         (struct transaction_request *)calloc(1, sizeof(struct transaction_request));

    if (!tr_req) {
      Nan::LowMemoryNotification();
      return Nan::ThrowError("Could not allocate memory.");
    }
    
    if (info.Length() < 1) {
      return Nan::ThrowError("Expecting Callback Function argument");
    }
    
    tr_req->conn = conn;
    tr_req->callback = new Nan::Callback(Local<Function>::Cast(info[0]));
    tr_req->type = rStart;
    
    conn->start_async();

    uv_work_t* req = new uv_work_t();
    req->data = tr_req;
    uv_queue_work(uv_default_loop(), req, EIO_TransactionRequest,  (uv_after_work_cb)EIO_After_TransactionRequest);
    
   // uv_ref(uv_default_loop());
    conn->Ref();
    
    return;
  } 
  
NAN_METHOD(Connection::QuerySync)
  {
    Nan::HandleScope scope;
    Connection *connection = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    int statement_type;
    if (info.Length() < 1 || !info[0]->IsString()){
       return Nan::ThrowError("Expecting a string query argument.");
    }
    
   
    String::Utf8Value *Query = new String::Utf8Value(info[0]->ToString()); 
    
    XSQLDA *sqlda = NULL;
    isc_stmt_handle stmt = NULL;
    bool r = connection->process_statement(&sqlda, **Query, &stmt, &statement_type);
    if(!r) {
      return Nan::ThrowError(
            String::Concat(Nan::New("In querySync - ").ToLocalChecked(),ERR_MSG(connection, Connection)));
    }
    
    Local<Value> argv[3];

    argv[0] = Nan::New<External>(sqlda);
    argv[1] = Nan::New<External>(&stmt);
    argv[2] = Nan::New<External>(connection);
    Local<Object> js_result(Nan::New(FBResult::constructor_template)->
                                     GetFunction()->NewInstance(3, argv));
    	
    if(statement_type==isc_info_sql_stmt_exec_procedure){
    	FBResult *fb_res = Nan::ObjectWrap::Unwrap<FBResult>(js_result);
    	Local<Value> js_value = fb_res->getCurrentRow(true);
    	info.GetReturnValue().Set(js_value);
    }
    

   info.GetReturnValue().Set(js_result);
        
  }
  
  
void Connection::EIO_After_Query(uv_work_t *req)
  {
   // uv_unref(uv_default_loop());
    Nan::HandleScope scope;
    struct query_request *q_req = (struct query_request *)(req->data);
	delete req;
   
	
    Local<Value> argv[3];
    if (!q_req->result) {
       argv[0] = Nan::Error(*Nan::Utf8String(
            String::Concat(Nan::New("While query - ").ToLocalChecked(),ERR_MSG(q_req->conn, Connection)))); 
       argv[1] = Nan::Null();        
    }
    else{
     argv[0] = Nan::New<External>(q_req->sqlda);
     argv[1] = Nan::New<External>(&q_req->stmt);
     argv[2] = Nan::New<External>(q_req->conn);
     
     Local<Object> js_result = Nan::New(FBResult::constructor_template)->GetFunction()->NewInstance(3, argv);
     
     if(q_req->statement_type==isc_info_sql_stmt_exec_procedure ){
    	 FBResult *fb_res = Nan::ObjectWrap::Unwrap<FBResult>(js_result);
    	 argv[1] = fb_res->getCurrentRow(true);
     }
     else  argv[1] = js_result;    
     argv[0] = Nan::Null();
    }
   
      
    TryCatch try_catch;
    
    q_req->callback->Call(2, argv);
    
    if (try_catch.HasCaught()) {
        node::FatalException(try_catch);
    }
    
    
    q_req->conn->stop_async();
    q_req->conn->Unref();
    free(q_req);
   // printf("qury finished \n");
   // scope.Close(argv[1]); 
    
  }
    
void Connection::EIO_Query(uv_work_t *req)
  {
    struct query_request *q_req = (struct query_request *)(req->data);
    
    q_req->result = q_req->conn->process_statement(&q_req->sqlda, **(q_req->Query), &q_req->stmt, &(q_req->statement_type));
    
    delete q_req->Query;
    //return;
  }
  
NAN_METHOD(Connection::Query)
  {
    Nan::HandleScope scope;
    Connection *conn = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    
    struct query_request *q_req =
         (struct query_request *)calloc(1, sizeof(struct query_request));

    if (!q_req) {
      Nan::LowMemoryNotification();
      return Nan::ThrowError("Could not allocate memory.");
    }
    
    if (info.Length() < 2 || !info[0]->IsString() ||
          !info[1]->IsFunction()) {
      return Nan::ThrowError("Expecting 1 string argument and 1 Function");
    }
    
    q_req->conn = conn;
    q_req->callback = new Nan::Callback(Local<Function>::Cast(info[1]));
    q_req->Query  = new String::Utf8Value(info[0]->ToString());
    q_req->sqlda = NULL;
    q_req->stmt = NULL;
    
    conn->start_async();

	uv_work_t* req = new uv_work_t();
    req->data = q_req;
    uv_queue_work(uv_default_loop(), req, EIO_Query,  (uv_after_work_cb)EIO_After_Query);
    
    
   //uv_ref(uv_default_loop());
    conn->Ref();
    return;
  }
  
NAN_METHOD(Connection::addEvent)
  {
    Nan::HandleScope scope;
    Connection *conn = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    
    if (info.Length() < 1 || !info[0]->IsString()) {
      return Nan::ThrowError("Expecting 1 string argument");
    }
    
    String::Utf8Value Event(info[0]->ToString());
    
    if(!event_block::FindBlock(conn->fb_events, *Event)){
        
       // event_block* eb;
        
      event_block::RegEvent(&(conn->fb_events), *Event, conn, &conn->db);
    }
    
   return;
  
  }
  
NAN_METHOD(Connection::deleteEvent)
  {
  
    Nan::HandleScope scope;
    Connection *conn = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    
    if (info.Length() < 1 || !info[0]->IsString()) {
      return Nan::ThrowError("Expecting 1 string argument");
    }
    
    String::Utf8Value Event(info[0]->ToString());
    
    event_block::RemoveEvent(&(conn->fb_events), *Event);
    
    return;

  }
  
NAN_METHOD(Connection::PrepareSync)
  {
    Nan::HandleScope scope;
    Connection *connection = Nan::ObjectWrap::Unwrap<Connection>(info.This());
        
    if (info.Length() < 1 || !info[0]->IsString()){
       return Nan::ThrowError("Expecting a string query argument.");
    }
    
    String::Utf8Value Query(info[0]->ToString());
    
    XSQLDA *insqlda = NULL;
    XSQLDA *outsqlda = NULL;
    isc_stmt_handle stmt = NULL;
    bool r = connection->prepare_statement(&insqlda,&outsqlda,*Query, &stmt);
    
    if(!r) {
      return Nan::ThrowError(
            String::Concat(Nan::New("In prepareSync - ").ToLocalChecked(),ERR_MSG(connection, Connection)));
    }
    
    Local<Value> argv[4];

    argv[0] = Nan::New<External>(insqlda);
    argv[1] = Nan::New<External>(outsqlda);
    argv[2] = Nan::New<External>(&stmt);
    argv[3] = Nan::New<External>(connection);
    Local<Object> js_result(Nan::New(FBStatement::constructor_template)->
                                     GetFunction()->NewInstance(4, argv));

    info.GetReturnValue().Set(js_result);
  }

NAN_METHOD(Connection::NewBlobSync)
  {
    Nan::HandleScope scope;
    Connection *conn = Nan::ObjectWrap::Unwrap<Connection>(info.This());
    Local<Value> argv[2];
    
    argv[0] = Nan::Null();
    argv[1] = Nan::New<External>(conn);
    
    Local<Object> js_blob(Nan::New(FBblob::constructor_template)->
                                     GetFunction()->NewInstance(2, argv));
    info.GetReturnValue().Set(js_blob);
  }
  
   Connection::Connection () : FBEventEmitter () 
  {
    db = NULL;
    trans = NULL;
    fb_events = NULL;
    connected = false;
  }

  Connection::~Connection ()
  { 
   // printf("connection free----------\n");
	if(db!=NULL) Close();
    assert(db == NULL);
  }
 
